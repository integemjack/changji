#include "http/server.hpp"

#include <crow.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>

#include "doctor/doctor.hpp"
#include "http/editing.hpp"
#include "http/media.hpp"
#include "http/upload.hpp"
#include "http/readonly.hpp"
#include "config/runtime.hpp"
#include "http/batch.hpp"
#include "http/config_api.hpp"
#include "http/episodes.hpp"
#include "http/llm_info.hpp"
#include "http/planning.hpp"
#include "http/projects.hpp"
#include "http/run.hpp"
#include "http/voices.hpp"
#include "http/scripting.hpp"
#include "llm/client.hpp"
#include "http/ws.hpp"
#include "comfy/client.hpp"
#include "infer/sd_image.hpp"
#include "pipeline/jobs.hpp"

namespace changji::http {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

/// 统一的 JSON 响应。
///
/// Crow 自带 crow::json，但整个项目其它地方用的是 nlohmann——
/// 混用两套 JSON 库是长期的麻烦源。这里统一序列化成字符串再交给 Crow。
crow::response json_response(const json& body, int status = 200) {
    crow::response res(status, body.dump());
    // **不带 charset。** FastAPI 发的就是这个，对拍比响应头时发现
    // 两边差一个 "; charset=utf-8"。JSON 按 RFC 8259 本来就必须是 UTF-8，
    // 这个参数在 application/json 上是没注册的，加了不算更对。
    // 差异出现在每一个接口上，而前面那几层对拍只比 body 不比头，一直没看见。
    res.set_header("Content-Type", "application/json");
    return res;
}

json to_json(const doctor::Report& report) {
    json checks = json::array();
    for (const auto& c : report.checks) {
        checks.push_back({
            {"name", c.name},
            {"level", doctor::to_string(c.level)},
            {"detail", c.detail},
            {"fix", c.fix},
        });
    }
    return {{"can_run", report.can_run()}, {"checks", checks}};
}

/// 取布尔查询参数。
///
/// 认的取值抄 FastAPI：true/1/on/yes/y/t，大小写不论。别的一律 false——
/// FastAPI 那边不认识的值是 422，但前端只会发 URLSearchParams 序列化出来的
/// "true"/"false"，为一个到不了的分支加一条错误路径不划算。
bool query_bool(const crow::request& req, const char* key, bool def = false) {
    const char* v = req.url_params.get(key);
    if (v == nullptr) return def;
    std::string s(v);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s == "true" || s == "1" || s == "on" || s == "yes" || s == "y" ||
           s == "t";
}

/// 取一个**必填**的查询参数。没这个键就抛 422。
///
/// **"没给"和"给了个空的"是两回事。** FastAPI 对 `path: str` 这种没有默认值
/// 的参数，缺了就在处理函数跑之前拦下来回 422；而 `?path=` 是一个合法的
/// 空字符串，会进处理函数然后回 400。少了这个区分的话，缺参数时 C++ 回的是
/// 404「没有剧集 」——注意末尾那个空格，那是拿空参数去查的结果。
///
/// 这一条是实时对拍抓出来的：单元测试直接调处理函数，天然绕过了这一层。
std::string required_query(const crow::request& req, const char* key) {
    const char* v = req.url_params.get(key);
    if (v == nullptr) throw unprocessable_query(key);
    return v;
}

/// 取查询参数。Crow 拿不到时返回 nullptr，转成空串——
/// 空串该怎么处理由各个接口自己决定（比如 path 为空是 400 不是 500）。
std::string query(const crow::request& req, const char* key) {
    const char* v = req.url_params.get(key);
    return v ? std::string(v) : std::string();
}

}  // namespace

void run(const config::Settings& settings, const Options& opts) {
    crow::SimpleApp app;

    // 起服务前先把配置放进 runtime。后面所有读配置的地方都从那里拿——
    // /api/connections 和 /api/settings 能在运行期改它，
    // 各处捕获一份的话，改完之后有的地方是新的有的是旧的。
    config::runtime().replace(settings);

    // job 表往 WebSocket 推消息，但它不认识 WebSocket——中间靠这个回调接上。
    // 分层的好处很实在：jobs.cpp 因此不用链 Crow，单元测试才编得动。
    pipeline::jobs().set_sink(
        [](const std::string& job_id, const json& msg) {
            ws::hub().broadcast(job_id, msg);
        });

    // 出图和出片的两个模型槽注册到调度器上。**一个进程只注册一次**：
    // 槽已经加载着的时候重新注册会抛异常，所以不能放在开跑的路径上。
    //
    // 注册不等于加载。真正加载要等第一次 acquire——一个视频模型好几个 G，
    // 起服务时就加载的话，只想看看分镜表的人也要等上几十秒。
    infer::register_sd_slots([] { return config::runtime().snapshot(); },
                             config::runtime().profile());

    // ---- REST ----

    CROW_ROUTE(app, "/api/health")([] {
        return json_response({{"ok", true}, {"service", "changji"}});
    });

    // 下面这些从 runtime 取而不是用 run() 收到的那份 settings：
    // /api/connections 和 /api/settings 能在运行期改配置，
    // 用捕获的那份的话，改完之后体检和硬件画像还是老的，
    // 用户会以为改动没生效。
    CROW_ROUTE(app, "/api/doctor")([] {
        const auto settings = config::runtime().snapshot();
        // 体检里有三项要发网络请求，最坏情况阻塞二十多秒。
        // Crow 是线程池模型，这只占住一个工作线程，不影响其它请求。
        return json_response(to_json(doctor::run_checks(settings)));
    });

    // ---- 阶段 2：只读接口 ----
    //
    // 处理逻辑放在 readonly.cpp 里的纯函数，这里只负责取查询参数和转响应。
    // 那些函数不碰 crow 类型，单元测试能不起服务就把它们跑一遍。

    CROW_ROUTE(app, "/api/hardware")([] {
        auto r = guard([] {
            return get_hardware(config::runtime().snapshot(),
                                config::runtime().profile());
        });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/settings")([] {
        auto r = guard([] {
            return get_settings(config::runtime().snapshot(),
                                config::runtime().profile());
        });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/projects")([] {
        auto r = guard([] {
            return get_projects(config::runtime().snapshot());
        });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/project")([](const crow::request& req) {
        auto r = guard([&] { return get_project(required_query(req, "path")); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/shots")([](const crow::request& req) {
        auto r = guard([&] {
            return get_shots(required_query(req, "path"),
                             required_query(req, "episode_id"));
        });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/assets")([](const crow::request& req) {
        auto r = guard([&] { return get_assets(required_query(req, "path")); });
        return json_response(r.body, r.status);
    });

    // ---- /api/media：带 Range 的文件服务 ----
    //
    // Range 是必须的，不是锦上添花：不实现的话前端 <video> 标签
    // 拖不动进度条，只能从头播。方案第三节点了名。

    CROW_ROUTE(app, "/api/media")([](const crow::request& req) {
        // 这条路不走 guard（它要回文件内容不是 JSON），所以必填参数
        // 的 422 要自己接住。
        const char* path_p = req.url_params.get("path");
        const char* rel_p = req.url_params.get("rel");
        if (path_p == nullptr || rel_p == nullptr) {
            const auto e =
                unprocessable_query(path_p == nullptr ? "path" : "rel");
            return json_response(e.detail(), e.status());
        }
        const auto t = resolve_media(path_p, rel_p);
        if (t.status != 200) {
            return json_response({{"detail", t.detail}}, t.status);
        }

        std::error_code ec;
        const auto size = static_cast<std::uint64_t>(fs::file_size(t.path, ec));
        if (ec) return json_response({{"detail", "读不到文件大小"}}, 500);

        std::ifstream in(t.path, std::ios::binary);
        if (!in) return json_response({{"detail", "打不开文件"}}, 500);

        const std::string ctype = content_type_for(t.path);
        const std::string range_header = req.get_header_value("Range");

        if (range_header.empty()) {
            std::string body((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
            crow::response res(200, std::move(body));
            res.set_header("Content-Type", ctype);
            // 没有这个头，浏览器不知道服务端支持分段，
            // 于是根本不会发 Range 请求，进度条照样拖不动。
            res.set_header("Accept-Ranges", "bytes");
            return res;
        }

        const auto r = parse_range(range_header, size);

        // **语法不认识和起点越界不是一回事。** 原来两种都回 416，
        // 对拍比出来 Python 那边前者回 400。对浏览器来说含义不同：
        // 416 带着文件真实长度，是"照这个重来"；400 是"你的请求是坏的"。
        if (r.verdict == RangeVerdict::Malformed) {
            crow::response res(400, "Range 头看不懂");
            res.set_header("Content-Type", "text/plain; charset=utf-8");
            return res;
        }
        if (r.verdict == RangeVerdict::NotSatisfiable) {
            // 416 必须带 Content-Range 告诉对方真实长度，而且 **body 是空的**
            // ——Python 那边 Content-Length 是 0，写点什么进去就对不上了。
            crow::response res(416, "");
            res.set_header("Content-Type", "text/plain; charset=utf-8");
            res.set_header("Content-Range", "bytes */" + std::to_string(size));
            return res;
        }
        if (r.verdict == RangeVerdict::Ignore) {
            // 多区间。见 media.hpp 里那段：当没看见这个头，回整个文件。
            std::string whole((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
            crow::response res(200, std::move(whole));
            res.set_header("Content-Type", ctype);
            res.set_header("Accept-Ranges", "bytes");
            return res;
        }

        const std::uint64_t len = r.range.last - r.range.first + 1;
        std::string chunk(static_cast<std::size_t>(len), '\0');
        in.seekg(static_cast<std::streamoff>(r.range.first));
        in.read(chunk.data(), static_cast<std::streamsize>(len));
        chunk.resize(static_cast<std::size_t>(in.gcount()));

        crow::response res(206, std::move(chunk));
        res.set_header("Content-Type", ctype);
        res.set_header("Accept-Ranges", "bytes");
        res.set_header("Content-Range",
                       "bytes " + std::to_string(r.range.first) + "-" +
                           std::to_string(r.range.last) + "/" +
                           std::to_string(size));
        return res;
    });

    // ---- 阶段 3：编辑接口 ----

    CROW_ROUTE(app, "/api/shot").methods("POST"_method)([](const crow::request& req) {
        auto r = guard([&] { return post_shot(json::parse(req.body, nullptr, false)); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/character").methods("POST"_method)([](const crow::request& req) {
        auto r = guard([&] { return post_character(json::parse(req.body, nullptr, false)); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/location").methods("POST"_method)([](const crow::request& req) {
        auto r = guard([&] { return post_location(json::parse(req.body, nullptr, false)); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/style").methods("POST"_method)([](const crow::request& req) {
        auto r = guard([&] { return post_style(json::parse(req.body, nullptr, false)); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/shots/batch").methods("POST"_method)([](const crow::request& req) {
        auto r = guard([&] { return post_shots_batch(json::parse(req.body, nullptr, false)); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/shots/reorder").methods("POST"_method)([](const crow::request& req) {
        auto r = guard([&] { return post_shots_reorder(json::parse(req.body, nullptr, false)); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/shots/link_locations").methods("POST"_method)([](const crow::request& req) {
        auto r = guard([&] { return post_shots_link_locations(json::parse(req.body, nullptr, false)); });
        return json_response(r.body, r.status);
    });

    // ---- 参考图上传（multipart）----
    //
    // multipart 的解析是 crow 的事，留在这一层；校验和落盘在 upload.cpp
    // 的纯函数里，那样不起服务也能测。

    CROW_ROUTE(app, "/api/character/reference").methods("POST"_method)
        ([](const crow::request& req) {
            auto r = guard([&]() -> ApiResult {
                crow::multipart::message msg(req);
                const auto field = [&](const char* name) -> std::string {
                    auto it = msg.part_map.find(name);
                    return it == msg.part_map.end() ? std::string() : it->second.body;
                };
                auto fit = msg.part_map.find("file");
                if (fit == msg.part_map.end()) throw ApiError(400, "没有上传文件");
                // 文件那一部分的 Content-Type 在它自己的头里，不是请求头里
                const std::string ctype =
                    fit->second.get_header_object("Content-Type").value;
                return post_character_reference(field("project"), field("char_id"),
                                                field("slot"), ctype,
                                                fit->second.body);
            });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/location/reference").methods("POST"_method)
        ([](const crow::request& req) {
            auto r = guard([&]() -> ApiResult {
                crow::multipart::message msg(req);
                const auto field = [&](const char* name) -> std::string {
                    auto it = msg.part_map.find(name);
                    return it == msg.part_map.end() ? std::string() : it->second.body;
                };
                auto fit = msg.part_map.find("file");
                if (fit == msg.part_map.end()) throw ApiError(400, "没有上传文件");
                const std::string ctype =
                    fit->second.get_header_object("Content-Type").value;
                return post_location_reference(field("project"), field("location_id"),
                                               ctype, fit->second.body);
            });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/character/reference/clear").methods("POST"_method)
        ([](const crow::request& req) {
            auto r = guard([&] {
                return post_character_reference_clear(
                    json::parse(req.body, nullptr, false));
            });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/location/reference/clear").methods("POST"_method)
        ([](const crow::request& req) {
            auto r = guard([&] {
                return post_location_reference_clear(
                    json::parse(req.body, nullptr, false));
            });
            return json_response(r.body, r.status);
        });

    // ---- 剧本 ----
    //
    // 三个都是同步的：调一次大模型，几十秒内返回。写整季不一样，
    // 那个要跑几分钟，走下面的 job 表。
    //
    // 客户端在这里造一次，三个路由共用。每个请求造一个的话，
    // 换成进程内 llama.cpp 之后就是每个请求重新加载一遍模型。
    // 传 provider 不是拷一份配置：/api/connections 能在运行期换大模型
    // 地址，拷一份的话改完之后这里还在往老地址发，而界面已经显示"已应用"了。
    static llm::RemoteClient script_client(
        [] { return config::runtime().snapshot().llm; },
        llm::default_http_post());

    const auto script_route = [](auto handler) {
        return [handler](const crow::request& req) {
            auto r = guard([&] {
                // 这几个接口没有自己的 job，取消令牌是个不会被触发的哑元。
                // 等它们接进 job 表之后换成真的那个。
                static thread_local pipeline::CancelToken tok;
                tok.reset();
                return handler(json::parse(req.body, nullptr, false),
                               script_client, tok);
            });
            return json_response(r.body, r.status);
        };
    };

    CROW_ROUTE(app, "/api/script/premise").methods("POST"_method)(
        script_route(&post_script_premise));
    CROW_ROUTE(app, "/api/script/write").methods("POST"_method)(
        script_route(&post_script_write));
    CROW_ROUTE(app, "/api/script/trailer").methods("POST"_method)(
        script_route(&post_script_trailer));

    // 出角色圣经和分镜表。走同一套包装。
    CROW_ROUTE(app, "/api/bible").methods("POST"_method)(
        script_route(&post_bible));
    CROW_ROUTE(app, "/api/plan").methods("POST"_method)(
        script_route(&post_plan));

    // ---- 连接设置与运行参数 ----
    //
    // 这三个是阶段 1 漏掉的，路由表核对时找出来的——前端那次探测只发
    // GET 请求，POST-only 的接口整片看不见。

    CROW_ROUTE(app, "/api/connections")([] {
        return json_response(get_connections().body);
    });

    CROW_ROUTE(app, "/api/connections").methods("POST"_method)(
        [](const crow::request& req) {
            static const auto check = default_doctor();
            auto r = guard([&] {
                return post_connections(json::parse(req.body, nullptr, false),
                                        check);
            });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/settings").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] {
                return post_settings(json::parse(req.body, nullptr, false));
            });
            return json_response(r.body, r.status);
        });

    // ---- 大模型接入信息 ----
    //
    // 方案的破契约白名单里原本有这两条，理由是"进程内推理之后语义重定义"。
    // 决策 4 之后那个理由不成立了：用远端大模型是长期形态之一，
    // 不是过渡状态。所以 providers 原样保留，models 是**扩展**不是替换——
    // 远端有哪些照旧列，另加一个字段列本机的 gguf。

    CROW_ROUTE(app, "/api/llm/providers")([] {
        return json_response(get_llm_providers().body);
    });

    CROW_ROUTE(app, "/api/llm/models")([] {
        static const auto fetch = default_http_get();
        auto r = guard([&] {
            return get_llm_models(config::runtime().snapshot(), fetch);
        });
        return json_response(r.body, r.status);
    });

    // ---- 根路径 ----
    //
    // **这是白名单里唯一真正的破契约。** Python 那边 GET / 返回
    // page.py 生成的一整页 HTML（那是删 Python 之前的内置界面）。
    // 两层架构下界面由 Node 提供，浏览器根本不会访问到这里——
    // 会撞上它的只有直接开了后端端口的人。给他们一句指路的话，
    // 比返回 404 或者一个空页面有用。
    CROW_ROUTE(app, "/")([&opts] {
        // 用原始字符串字面量，换行直接写在源码里。
        const std::string body =
            std::string(R"(场记 C++ 后端在跑。
这里只有接口，界面在 Node 那一层——默认 http://127.0.0.1:5174

体检： http://127.0.0.1:)") + std::to_string(opts.port) + "/api/doctor\n";
        crow::response res(200, body);
        res.set_header("Content-Type", "text/plain; charset=utf-8");
        return res;
    });

    // ---- 项目的新建、删除、改梗概 ----
    //
    // 同样是阶段 3 漏掉的。删项目那个不可逆，三道闸在 projects.cpp 里。

    CROW_ROUTE(app, "/api/new").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] {
                return post_new_project(json::parse(req.body, nullptr, false),
                                        config::runtime().snapshot());
            });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/project/delete").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] {
                return post_delete_project(json::parse(req.body, nullptr, false),
                                           config::runtime().snapshot());
            });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/project/premise").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] {
                return post_project_premise(json::parse(req.body, nullptr, false));
            });
            return json_response(r.body, r.status);
        });

    // ---- 剧本读写与剧集增删改 ----
    //
    // 这几个属于阶段 3，当时按 test_web_editing.py 的覆盖面移植而漏了。
    // 阶段 2 判据的实机验证里前端调出 404 才发现。

    CROW_ROUTE(app, "/api/script")([](const crow::request& req) {
        auto r = guard([&] {
            return get_script(required_query(req, "path"),
                              required_query(req, "episode_id"));
        });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/script").methods("POST"_method)(
        script_route(&post_script));

    CROW_ROUTE(app, "/api/episode").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] {
                return post_episode(json::parse(req.body, nullptr, false));
            });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/episode/action").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] {
                return post_episode_action(json::parse(req.body, nullptr, false));
            });
            return json_response(r.body, r.status);
        });

    // ---- 两个长任务 ----
    //
    // 立刻返回 {"started": true, ...}，活干在工作线程上。
    // 进度靠 GET /api/script/series 轮询或者 WebSocket 推。
    //
    // 客户端换成 shared_ptr：任务比这次请求活得久，
    // 上面那个 static 引用在这里不够安全——将来换成按项目建的客户端时，
    // 引用会在任务还跑着的时候失效。
    static auto batch_client = std::make_shared<llm::RemoteClient>(
        llm::ConfigProvider([] { return config::runtime().snapshot().llm; }),
        llm::default_http_post());

    const auto batch_route = [](auto handler) {
        return [handler](const crow::request& req) {
            auto r = guard([&] {
                return handler(json::parse(req.body, nullptr, false),
                               batch_client);
            });
            return json_response(r.body, r.status);
        };
    };

    CROW_ROUTE(app, "/api/script/series").methods("POST"_method)(
        batch_route(&post_script_series));
    CROW_ROUTE(app, "/api/plan/all").methods("POST"_method)(
        batch_route(&post_plan_all));

    // ---- 任务状态与开跑 ----

    CROW_ROUTE(app, "/api/run")([] {
        return json_response(pipeline::jobs().snapshot(pipeline::JobKind::Run));
    });

    // 开跑之前先看看这一次会做什么、大概多久。一按就是几十分钟，
    // 哪些镜头会重做应该在按下去之前就知道。
    //
    // 画像走 runtime 而不是自己 detect()：用户在设置页改过的画质档位
    // 要算进预估里，不然改完分辨率预演的时间不变，看着像是没生效。
    CROW_ROUTE(app, "/api/run/preview")([](const crow::request& req) {
        auto r = guard([&] {
            return get_run_preview(required_query(req, "path"),
                                   query(req, "episode_id"),
                                   query_bool(req, "all_episodes"),
                                   query_bool(req, "skip_final"),
                                   query_bool(req, "force"),
                                   config::runtime().profile());
        });
        return json_response(r.body, r.status);
    });

    // 服务端有哪些参考音色。角色页打开时顺带拉一次。
    //
    // 客户端每次现建：ComfyUI 的地址能在设置页改，存一份的话改完不生效。
    // object_info 的缓存也跟着丢，但这个接口一次请求就问一遍，
    // 缓存本来就用不上。
    CROW_ROUTE(app, "/api/voices")([](const crow::request& req) {
        auto r = guard([&] {
            comfy::Client client(
                [] { return config::runtime().snapshot().comfy; },
                comfy::default_transport(
                    [] { return config::runtime().snapshot().comfy; }));
            return get_voices(required_query(req, "path"), client);
        });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/outputs")([](const crow::request& req) {
        auto r = guard([&] { return get_outputs(required_query(req, "path")); });
        return json_response(r.body, r.status);
    });

    // 开跑。立刻返回，进度靠上面那个轮询或者 WebSocket 推。
    // 后端每次开跑现取（配置可能刚被改过），所以 deps 不在这里存一份。
    CROW_ROUTE(app, "/api/run").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] {
                return post_run(json::parse(req.body, nullptr, false),
                                default_run_deps());
            });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/stop").methods("POST"_method)([](const crow::request&) {
        // 没在跑时回 {"stopped": false} 而不是报错。
        // 前端的停止按钮是无条件可点的，重复点不该弹错误框。
        const bool stopped = pipeline::jobs().cancel(pipeline::JobKind::Run);
        return json_response({{"stopped", stopped}});
    });

    CROW_ROUTE(app, "/api/script/series")([] {
        return json_response(pipeline::jobs().snapshot(pipeline::JobKind::Write));
    });

    CROW_ROUTE(app, "/api/script/series/stop").methods("POST"_method)
        ([](const crow::request&) {
            const bool stopped = pipeline::jobs().cancel(pipeline::JobKind::Write);
            return json_response({{"stopped", stopped}});
        });

    // ---- WebSocket ----
    //
    // 进度消息由 job 表通过上面那个 sink 推过来。
    // 前端可以只连 WebSocket，也可以继续轮询 /api/run——两条路并存，
    // WebSocket 断了退回轮询就行，任务本身不受影响。

    CROW_WEBSOCKET_ROUTE(app, "/ws")
        .onopen([](crow::websocket::connection& conn) {
            ws::hub().add(&conn);
            conn.send_text(json{{"type", "hello"},
                                {"service", "changji"}}.dump());
        })
        .onclose([](crow::websocket::connection& conn,
                    const std::string& /*reason*/) {
            // 形参个数跟 Crow 版本走：1.2.0 是 (connection&, reason)，
            // 更新的版本多一个 uint16_t 关闭码。升级 Crow 时这里会编译报错，
            // 那是好事——静默的签名不匹配会让 onclose 根本不被调用，
            // 连接泄漏在注册表里，直到某次广播向已析构的对象发送才崩。
            ws::hub().remove(&conn);
        })
        .onmessage([](crow::websocket::connection& conn,
                      const std::string& data, bool is_binary) {
            if (is_binary) return;  // 上行没有二进制消息，忽略
            ws::hub().handle_client_message(&conn, data);
        });

    CROW_LOG_INFO << "changji 监听 " << opts.host << ":" << opts.port;

    app.bindaddr(opts.host)
        .port(static_cast<std::uint16_t>(opts.port))
        .concurrency(opts.concurrency)
        .run();
}

}  // namespace changji::http
