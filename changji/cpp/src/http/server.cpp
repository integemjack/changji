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
#include "config/writeback.hpp"
#include "http/batch.hpp"
#include "http/config_api.hpp"
#include "http/episodes.hpp"
#include "http/llm_info.hpp"
#include "http/planning.hpp"
#include "http/projects.hpp"
#include "http/run.hpp"
#include "http/voices.hpp"
#include "http/scripting.hpp"
#include "http/setup_api.hpp"
#include "llm/client.hpp"
#include "infer/llama_chat.hpp"
#include "llm/local_client.hpp"
#include "http/flow.hpp"
#include "util/paths.hpp"
#include "http/webapp.hpp"
#include "http/ws.hpp"
#include "infer/scheduler.hpp"
#include "infer/sd_backend.hpp"
#include "infer/sd_image.hpp"
#include "models/hardware.hpp"
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
    // 同工作进程那边：不接的话 sd.cpp 一条日志都不会落地，
    // 而出图失败时抛的是"看一眼上面 sd.cpp 打的日志"。
    infer::sd_log_to_stderr();
    infer::register_sd_slots([] { return config::runtime().snapshot(); },
                             config::runtime().profile());
    // [llm].backend = "local" 时把大模型也挂上调度器。远端那条不注册——
    // 没有本地权重，注册一个装不上的槽只会在借它时抛没意义的错。
    llm::register_llm_slot([] { return config::runtime().snapshot(); },
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
        auto r = guard([&] { return post_shot(parse_body(req.body)); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/character").methods("POST"_method)([](const crow::request& req) {
        auto r = guard([&] { return post_character(parse_body(req.body)); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/location").methods("POST"_method)([](const crow::request& req) {
        auto r = guard([&] { return post_location(parse_body(req.body)); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/style").methods("POST"_method)([](const crow::request& req) {
        auto r = guard([&] { return post_style(parse_body(req.body)); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/shots/batch").methods("POST"_method)([](const crow::request& req) {
        auto r = guard([&] { return post_shots_batch(parse_body(req.body)); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/shots/reorder").methods("POST"_method)([](const crow::request& req) {
        auto r = guard([&] { return post_shots_reorder(parse_body(req.body)); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/shots/link_locations").methods("POST"_method)([](const crow::request& req) {
        auto r = guard([&] { return post_shots_link_locations(parse_body(req.body)); });
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
                    parse_body(req.body));
            });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/location/reference/clear").methods("POST"_method)
        ([](const crow::request& req) {
            auto r = guard([&] {
                return post_location_reference_clear(
                    parse_body(req.body));
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
    // **两条后端都要能走，而且要在这里选一次。**
    // 每个请求选一次的话，local 那条每次都要重新借槽——借槽本身不贵，
    // 但把"选哪条"散到各个路由里，将来加第三条后端就要改三处。
    static std::shared_ptr<llm::Client> script_client =
        llm::make_client(llm::default_http_post());

    const auto script_route = [](auto handler) {
        return [handler](const crow::request& req) {
            auto r = guard([&] {
                // 这几个接口没有自己的 job，取消令牌是个不会被触发的哑元。
                // 等它们接进 job 表之后换成真的那个。
                static thread_local pipeline::CancelToken tok;
                tok.reset();
                return handler(parse_body(req.body),
                               *script_client, tok);
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
                return post_connections(parse_body(req.body),
                                        check);
            });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/settings").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] {
                return post_settings(parse_body(req.body));
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
    // ---- Node 那个 BFF 的几条，引擎自己也答一份 ----
    //
    // **为什么引擎要管这个。** 引擎自己发前端之后，走这条路的人拿不到
    // `/bff/*`——界面能打开、项目列表也在，但**剧集下拉框是空的**、
    // 顶栏写着"引擎连不上"，制作页点不动。那等于"打开端口就能看处理进度"
    // 没做完。
    //
    // 只搬**判定和状态**这几条。投递（`/bff/publish/*`）没搬：它要存投递
    // 记录、要平台配置，是另一套东西，不是顺手能带的。要投递就仍然起 Node。

    CROW_ROUTE(app, "/bff/health")([] {
        return json_response({{"ok", true}, {"service", "changji"}});
    });

    // 前端拿它点亮顶栏那个"引擎连不上/已连接"。
    // **由引擎自己答的时候它恒为在线**——答得出这个请求就说明活着，
    // 再去 ping 自己一次没有意义。
    CROW_ROUTE(app, "/bff/settings/status")([] {
        return json_response({{"online", true},
                              {"baseUrl", ""},
                              {"latencyMs", 0},
                              {"service", "changji"}});
    });

    CROW_ROUTE(app, "/bff/settings/config")([] {
        const auto s = config::runtime().snapshot();
        json locked = json::object();
        for (const auto& [k, v] : config::env_overridden()) locked[k] = v;
        // **embedded 告诉前端"引擎就是我自己"。**
        // 由引擎自己答这个请求时，"引擎地址"和"请求超时"两个输入框是空的、
        // 也没有意义（改了也没人读），显示出来只会让人以为哪里没配好。
        // 起 Node 那层转发时答的是另一份，带真地址，那时才该显示。
        return json_response(
            {{"embedded", true},
             {"engineBaseUrl", ""},
             {"engineTimeoutMs", 0},
             {"configFile", changji::paths::to_utf8(config::user_config_path())},
             {"envLocked", locked}});
    });

    // **设置页真正用的是这一条**，不是上面那两条。
    //
    // 漏了它的后果很难看懂：顶栏读 /bff/settings/status 显示"引擎已连接"，
    // 而设置页读这一条拿到 404，于是整页当引擎离线处理——地址空、
    // 配置文件空、右上角写"连不上"。**同一个页面上两个相反的结论。**
    //
    // 形状照抄 Node 那份（webapp/server/src/routes/settings.js）：它要去
    // 发四次 HTTP，我们在进程内直接取。errors 留空对象——那几项是它
    // 转发失败时填的，我们没有转发这一层。
    CROW_ROUTE(app, "/bff/settings/overview")([] {
        json locked = json::object();
        for (const auto& [k, v] : config::env_overridden()) locked[k] = v;
        const auto s = config::runtime().snapshot();

        json out;
        out["node"] = {
            {"embedded", true},
            {"engineBaseUrl", ""},
            {"engineTimeoutMs", 0},
            {"configFile", changji::paths::to_utf8(config::user_config_path())},
            // **这两条走 bff 而不是 /api/connections。** 那个接口在对拍
            // 覆盖范围内，Python 没有这两个字段，加进去就是一处破契约。
            // bff 这一层是我们自己的，前端要拿它决定哪些输入框该显示——
            // backend = local 时"API 地址/模型名/密钥"三项一个都不读，
            // 摆着只会让人调了没反应。
            {"llmBackend", s.llm.backend},
            {"ttsBackend", s.tts.backend},
            {"envLocked", locked}};
        // 由引擎自己答就说明它活着，再 ping 自己一次没有意义。
        out["engine"] = {{"online", true},
                         {"baseUrl", ""},
                         {"latencyMs", 0},
                         {"service", "changji"}};
        out["connections"] = get_connections().body;
        out["settings"] = get_settings(s).body;
        out["hardware"] =
            get_hardware(s, config::runtime().profile()).body;
        // **这一轮真正会用的规格。C++ 独有，所以放在 /bff。**
        //
        // `hardware.tiers.final` 是档位表按显存推出来的，出片时会被两件事
        // 盖掉：画幅来自项目的 [video]，步数在挂了 Turbo 时压到 6。
        // 设置页照着档位表显示的话，写的是"成片步数 28"而实际跑 6 步——
        // 用户看了会问"怎么没用 turbo"。真发生过。
        //
        // 和 run.cpp 调的是同一个函数，两边不会分叉。
        {
            const auto& prof = config::runtime().profile();
            int table = 0;
            if (const auto it = prof.tiers.find(models::Tier::FINAL);
                it != prof.tiers.end()) {
                table = it->second.steps;
            }
            const auto eff = config::effective_spec(s, table);
            out["effective"] = {{"width", eff.width},
                                {"height", eff.height},
                                {"finalSteps", eff.final_steps},
                                {"frameSteps", eff.frame_steps},
                                {"turbo", eff.turbo},
                                {"stepsPinned", eff.steps_pinned},
                                {"tableSteps", table}};
            // **显卡真有多少显存，和配置里顶着的那个数分开给。**
            // /api/hardware 的 vram_gb 在有 vram_gb_override 时回的是 override
            // （Python 就这样，对拍不能动），于是设置页写着 "5090 · 12 GB"——
            // 用户说"硬件 GPU 显存有获取不准的 bug"。探到的数单独回，
            // 顶着的那个也回，界面把两件事说清楚。
            out["effective"]["physicalVramGb"] =
                prof.gpu.has_value() ? json(prof.gpu->vram_gb()) : json(nullptr);
            out["effective"]["vramOverride"] =
                s.vram_gb_override.has_value() ? json(*s.vram_gb_override) : json(nullptr);
            // **程序给两个模型算出来的权重放置，显示给用户看。**
            //
            // weights 已经不让人在界面上填了（用户："都应该让程序自己算"），
            // 但算完了不给看是另一个极端：出图慢到底是卡不行还是权重在内存里
            // 每步搬一趟，用户没有任何线索。2026-09-10 排查 GPU 利用率只有
            // 18% 那次，答案就是这一项——当时得连上机器看日志才知道。
            //
            // 和出图出片建上下文走的是同一个 expand_placement，不会分叉。
            {
                const double card_gb = prof.gpu.has_value() ? prof.gpu->vram_gb()
                                                            : prof.vram_gb;
                const auto ex = config::expand_placement(s, card_gb);
                const auto pack = [](const config::PlacementInfo& p) {
                    return json{{"weights", p.weights},
                                {"modelGb", p.model_gb},
                                {"liveVramGb", p.live_vram_gb},
                                {"resident", p.resident}};
                };
                // **实测占用也给出来。** 估算和真实差得离谱（video 那一路
                // 算 14.6 GB、实测 74 GB），界面上只显示估算会误导人。
                // 量到之前是 null——那时候调度器走的也正是保守估算。
                const auto measured = [](infer::Slot sl) {
                    const std::size_t b = infer::scheduler().measured_vram(sl);
                    return b == 0 ? json(nullptr)
                                  : json(static_cast<double>(b) /
                                         (1024.0 * 1024 * 1024));
                };
                json vid = pack(config::video_placement(ex));
                json img = pack(config::image_placement(ex));
                vid["measuredVramGb"] = measured(infer::Slot::Video);
                img["measuredVramGb"] = measured(infer::Slot::Image);
                out["effective"]["placement"] = {
                    {"video", vid}, {"image", img}, {"cardGb", card_gb}};
            }
        }
        // **体检要发网络请求，最坏二十多秒。** Node 那份也是同步等的，
        // 形状要一致就只能照做；Crow 是线程池，占住一个工作线程不影响别的请求。
        out["doctor"] = to_json(doctor::run_checks(s));
        out["errors"] = json::object();
        return json_response(out);
    });

    // ---- 大模型跑在哪：内置还是外接 ----
    //
    // 走 /bff 不走 /api/connections：那个接口在对拍覆盖范围内，
    // Python 没有 llm.backend 这个字段。
    //
    // **两条都要留着。** 默认内置（一个程序跑所有），但本机跑不动大模型的、
    // 想用云上更强模型的、团队共用一台推理机的，都要能切到外接。
    CROW_ROUTE(app, "/bff/settings/llm")
        .methods("POST"_method)([](const crow::request& req) {
            auto r = guard([&] {
                const auto body = parse_body(req.body);
                const auto it = body.find("backend");
                if (it == body.end() || !it->is_string()) {
                    throw ApiError(400, "缺 backend");
                }
                const auto backend = it->get<std::string>();
                if (backend != "local" && backend != "remote") {
                    throw ApiError(400, "backend 只能是 local 或 remote");
                }
                if (backend == "local" && !infer::llama_chat_available()) {
                    // **说清是构建选项，不是配置写错了。** 只说"不支持"
                    // 的话用户会去翻配置文件找哪里填错了。
                    throw ApiError(
                        400,
                        "这个二进制没编进程内大模型（构建时 "
                        "CHANGJI_LLAMA=OFF）。用外接：backend = remote "
                        "并填 [llm].base_url");
                }
                config::save_user_config(json{{"llm", {{"backend", backend}}}});
                auto s = config::runtime().snapshot();
                s.llm.backend = backend;
                config::runtime().replace(s);
                return ApiResult{200, {{"backend", backend}}};
            });
            return json_response(r.body, r.status);
        });

    // ---- 这部剧的画面规格 ----
    //
    // 竖屏还是横屏、720p 还是 2K。**一部剧一份**，写在项目目录的
    // changji.toml 里——一台机器上可以同时有竖屏短剧和横屏片子。
    //
    // 走 /bff 不走 /api：这两项是 C++ 独有的，而 /api/project 那份 JSON
    // 在对拍覆盖范围内，Python 没有它们。
    // ---- 这一轮还没落定的镜头 ----
    //
    // **C++ 独有，所以在 /bff 不在 /api。** 镜头墙上的「排队中」原来只存在
    // 浏览器内存里：刷新一下、换个标签页、换台设备，排着的全没了，正在跑的
    // 那一镜也要等到下一条进度才亮。引擎自己一直知道这一轮还有哪几镜没跑完
    // （每个阶段开工登记一批，每落定一镜划掉一个）——页面一进来问它就是了。
    CROW_ROUTE(app, "/bff/run/pending")([] {
        return json_response(
            {{"running", pipeline::jobs().running(pipeline::JobKind::Run)},
             {"shot_ids", pipeline::jobs().pending(pipeline::JobKind::Run)}});
    });

    // ---- 首次运行：把模型下下来 ----
    //
    // **C++ 独有，所以在 /bff 不在 /api。** Python 那边模型是 ComfyUI 管的，
    // 它根本不知道文件在哪，更没有"下模型"这件事。
    //
    // 装好程序之后 `[models]` 是空的，界面能打开、项目能建，点到「出片」
    // 才发现什么都跑不了——那时候用户手上只有一句"本地模型一个都没配"
    // 和一个配置文件路径。这四条接口把那段路变成"看推荐、点下载、等"。

    CROW_ROUTE(app, "/bff/setup/state")([] {
        auto r = guard([&] {
            return get_setup_state(config::runtime().snapshot(),
                                   config::runtime().profile());
        });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/bff/setup/download")
        .methods("POST"_method)([](const crow::request& req) {
            auto r = guard([&] {
                return post_setup_download(config::runtime().snapshot(),
                                           parse_body(req.body));
            });
            return json_response(r.body, r.status);
        });

    // 前端一秒问一次。**故意不走 WebSocket**：进度是一个可以随时重新问出来
    // 的状态，不是一串必须收全的事件。刷新页面、换台设备、下到一半关掉浏览器
    // 第二天回来——轮询这三种都对，而事件流每一种都要另写一段补偿。
    CROW_ROUTE(app, "/bff/setup/progress")([] {
        auto r = guard([&] { return get_setup_progress(); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/bff/setup/cancel")
        .methods("POST"_method)([](const crow::request&) {
            auto r = guard([&] { return post_setup_cancel(); });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/bff/project/video")([](const crow::request& req) {
        auto r = guard([&] {
            const auto root =
                changji::paths::from_utf8(required_query(req, "path"));
            const auto s = config::load_settings(root);
            const auto [w, h] = s.video.size();
            return ApiResult{200,
                             {{"orientation", s.video.orientation},
                              {"quality", s.video.quality},
                              // 把算出来的尺寸也回去：界面上要显示
                              // "720p 竖屏 = 704×1280"，用户才知道自己选的
                              // 到底是多大。
                              {"width", w},
                              {"height", h}}};
        });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/bff/project/video")
        .methods("POST"_method)([](const crow::request& req) {
            auto r = guard([&] {
                const auto body = parse_body(req.body);
                const auto pick = [&body](const char* k) {
                    const auto it = body.find(k);
                    if (it == body.end() || !it->is_string()) {
                        throw ApiError(400, std::string("缺 ") + k);
                    }
                    return it->get<std::string>();
                };
                const auto root = changji::paths::from_utf8(pick("path"));
                config::VideoConfig v;
                v.orientation = pick("orientation");
                v.quality = pick("quality");
                // **先校验再写。** 写进去再报错的话，文件已经坏了，
                // 而下一次加载会整个失败——那时候连界面都打不开。
                const auto errs = v.validate();
                if (!errs.empty()) {
                    std::string msg;
                    for (const auto& e : errs) {
                        if (!msg.empty()) msg += "；";
                        msg += e;
                    }
                    throw ApiError(400, msg);
                }

                config::save_user_config(
                    json{{"video",
                          {{"orientation", v.orientation},
                           {"quality", v.quality}}}},
                    root / "changji.toml");
                const auto [w, h] = v.size();
                return ApiResult{200,
                                 {{"orientation", v.orientation},
                                  {"quality", v.quality},
                                  {"width", w},
                                  {"height", h}}};
            });
            return json_response(r.body, r.status);
        });

    // ---- 投递（第八步）：这一套没搬进来 ----
    //
    // 它要存投递记录、要平台配置和凭据，是另一套东西。**但不能就这么
    // 404**：前端 `Promise.all([api.platforms(), api.publishTargets()])`
    // 一挂，整页就是一个红框，用户不知道是"没做"还是"坏了"。
    //
    // 回一个合法的空形状加一句说明——页面画得出来，而且说得清为什么是空的。
    const auto publish_not_here = [](const char* field) {
        return [field] {
            json body{{field, json::array()},
                      {"error",
                       "投递功能要起 webapp 那层 Node 服务（webapp/server）。"
                       "这个二进制自带的是制作那七步，投递没搬进来——"
                       "它要存投递记录和各平台的凭据，是另一套东西。"}};
            return json_response(body);
        };
    };
    CROW_ROUTE(app, "/bff/publish/platforms")(publish_not_here("platforms"));
    CROW_ROUTE(app, "/bff/publish/targets")(publish_not_here("targets"));
    CROW_ROUTE(app, "/bff/publish/records")(publish_not_here("records"));

    // 写那几条直接说清楚。回 501 而不是 404：404 像"地址写错了"，
    // 501 是"这条路存在但这个部署没实现"，而后者才是实情。
    const auto publish_write = [](const crow::request&) {
        return json_response(
            {{"detail",
              "投递要起 webapp 那层 Node 服务（webapp/server），"
              "这个二进制没带这一套。"}},
            501);
    };
    CROW_ROUTE(app, "/bff/publish/targets").methods("POST"_method)(publish_write);
    CROW_ROUTE(app, "/bff/publish/deliver").methods("POST"_method)(publish_write);
    CROW_ROUTE(app, "/bff/publish/batch").methods("POST"_method)(publish_write);
    // 删投递目标：前端拼的是 /bff/publish/targets/<id>。
    CROW_ROUTE(app, "/bff/publish/targets/<string>")
        .methods("DELETE"_method)(
            [publish_write](const crow::request& req, const std::string&) {
                return publish_write(req);
            });

    // 八步走到哪一步了。
    CROW_ROUTE(app, "/bff/flow")([](const crow::request& req) {
        const char* raw_path = req.url_params.get("path");
        json body{{"steps", flow_steps()}};
        if (raw_path == nullptr || *raw_path == 0) {
            // 还没选项目：八步全未完成，前端照样画得出侧边栏
            json done = json::object();
            for (const auto& st : flow_steps()) done[st["key"]] = false;
            body["done"] = done;
            body["counters"] = json::object();
            body["project"] = nullptr;
            body["episode"] = nullptr;
            return json_response(body);
        }
        const std::string path = raw_path;

        auto proj = guard([&] { return get_project(path); });
        if (proj.status != 200) {
            return json_response(proj.body, proj.status);
        }
        const json& project = proj.body;

        // 没指定就跟第一集走，和 Node 那边一样
        std::string episode_id;
        if (const char* e = req.url_params.get("episode_id")) episode_id = e;
        const auto& eps = project.contains("episodes") && project["episodes"].is_array()
                              ? project["episodes"]
                              : json::array();
        if (episode_id.empty() && !eps.empty() && eps[0].is_object()) {
            episode_id = eps[0].value("episode_id", std::string());
        }
        json episode = nullptr;
        for (const auto& e : eps) {
            if (e.is_object() && e.value("episode_id", std::string()) == episode_id) {
                episode = e;
                break;
            }
        }

        // 分镜和产物取不到就当空的——**别让整条判定挂掉**。
        // 一个还没出分镜的项目本来就该走到"分镜"那一步停下，
        // 而不是让侧边栏整个不显示。
        json shots = json::array();
        if (!episode_id.empty()) {
            auto r = guard([&] { return get_shots(path, episode_id); });
            if (r.status == 200 && r.body.contains("shots") &&
                r.body["shots"].is_array()) {
                shots = r.body["shots"];
            }
        }
        json outputs = json::array();
        {
            auto r = guard([&] { return get_outputs(path); });
            if (r.status == 200 && r.body.contains("files") &&
                r.body["files"].is_array()) {
                outputs = r.body["files"];
            }
        }

        const json assessed = flow_assess(project, shots, outputs, episode_id);
        body["done"] = assessed["done"];
        body["counters"] = assessed["counters"];
        body["project"] = project;
        body["episodeId"] = episode_id;
        body["episode"] = episode;
        body["outputs"] = outputs;
        return json_response(body);
    });

    // ---- 前端 ----
    //
    // **这里以前回的是一句 text/plain 指路**（"界面在 Node 那一层，
    // 默认 5174"）。那是迁移期的临时状态，代价是用户得起两个进程，
    // 而且打开引擎的端口看不到任何东西。现在把打包好的前端嵌进二进制
    // 直接发（见 http/webapp.hpp）。
    //
    // `/bff/*` 现在也在上面答了（清单见 bff_routes.hpp），所以整个界面
    // 都能用，不用再起 Node 那一层。
    //
    // **逐字段填 res，不要整个赋值。**
    // catchall 拿到的 `crow::response&` 已经带着这次连接的内部状态，
    // `res = 另一个 response` 会把那些状态一起覆盖掉——浏览器收到的是
    // ERR_CONTENT_LENGTH_MISMATCH，而服务端日志里明明白白写着 200。
    // 日志说成功、浏览器说坏掉，这种最难查。
    const auto fill_webapp = [](const crow::request& req, crow::response& res) {
        const std::string rel = normalize_webapp_path(req.url);
        if (rel.empty()) {
            // 只有 `..` 这类会走到这儿
            res.code = 400;
            res.body = "路径不合法";
            res.set_header("Content-Type", "text/plain; charset=utf-8");
            return;
        }
        const std::string* body = find_webapp_file(rel);
        std::string name = rel;
        if (!body && wants_file(rel)) {
            // **要的是具体文件却没有，就老老实实 404。**
            //
            // 以前这里也回 index.html，代价很大：浏览器按 <script> 去取
            // assets/index-旧哈希.js，拿回来的是一整页 HTML，还是 200。
            // 解析当场失败、页面全白，而 F12 里每一个请求都是 200，
            // 日志里也全是 200 —— 没有任何东西提示出了错。
            // 换一版之后浏览器还拿着旧 index.html 的时候就是这个场面。
            res.code = 404;
            res.body = "没有这个文件：" + rel;
            res.set_header("Content-Type", "text/plain; charset=utf-8");
            res.set_header("Cache-Control", "no-store");
            return;
        }
        if (!body) {
            // 前端路由（/shots 这种，没有扩展名）回 index.html：
            // 单页应用的深链接靠这个。
            body = find_webapp_file("index.html");
            name = "index.html";
        }
        if (!body) {
            res.code = 500;
            res.body =
                "前端没打包进来。生成一下：cd webapp/client && npm run build，"
                "然后 python cpp/tools/gen_webapp.py，再重编。";
            res.set_header("Content-Type", "text/plain; charset=utf-8");
            return;
        }
        res.code = 200;
        res.body = *body;
        res.set_header("Content-Type", webapp_content_type(name));
        // 带哈希的资源长期缓存，index.html 每次核一遍。见 webapp_cache_control。
        res.set_header("Cache-Control", webapp_cache_control(name));
    };

    const auto webapp_route = [fill_webapp](const crow::request& req) {
        crow::response res;
        fill_webapp(req, res);
        return res;
    };
    CROW_ROUTE(app, "/")(webapp_route);

    // **静态资源走普通路由，不走 catchall。**
    // 实测：同一个文件从 catchall 出去只有 65389 字节，从普通路由出去是
    // 完整的 131332——浏览器报 ERR_CONTENT_LENGTH_MISMATCH，页面一片空白，
    // 而服务端日志两次都写 200。原因没查到底（Crow 的 catchall 那条路上
    // 大 body 被截断），但普通路由是好的，就走普通路由。
    // catchall 只留给单页应用兜底，那里只发几百字节的 index.html。
    CROW_ROUTE(app, "/assets/<path>")(
        [webapp_route](const crow::request& req, const std::string&) {
            return webapp_route(req);
        });

    // **单段的前端路由也走普通路由。**
    //
    // 2026-09-11 实测：catchall **把 body 整个丢了**——`/project` 回
    // 200 但 0 字节，`/nope.js` 回 404 也是 0 字节，而同样的内容从普通
    // 路由出去是完整的。上面那条注释记的"从 catchall 出去只有 65389 字节"
    // 是同一个毛病，当时以为只有大 body 受影响，就把静态资源挪走了、
    // 把单页应用的兜底留下了。**留下的那半正是白屏**：
    // 从 `/` 进去能用，一刷新（或者直接打开 /shots）就是空白页，
    // 而且刷多少次都一样——body 压根没发出来。
    //
    // `<string>` 只吃一段，不含斜杠，所以 `/api/run` 这类两段的接口不会被
    // 它抢走（上面那条注释说的 `<path>` 会吃斜杠，才是不能用的那个）。
    // 现在所有前端路由都是单段：/project、/shots、/settings……
    CROW_ROUTE(app, "/<string>")(
        [fill_webapp](const crow::request& req, const std::string&) {
            crow::response res;
            if (!webapp_owns(req.url)) {
                // 单段的接口路径没匹配上，照常回 404 JSON，别拿 index.html 顶
                res.code = 404;
                res.body = R"({"detail":"Not Found"})";
                res.set_header("Content-Type", "application/json");
                return res;
            }
            fill_webapp(req, res);
            return res;
        });

    // **兜底用 CROW_CATCHALL_ROUTE，不能用 `/<path>`。**
    // Crow 的 `<path>` 连斜杠一起吃，写成路由的话它会把 `/api/outputs`、
    // `/api/run` 这些全匹配走——对拍当场报出来：Python 回 200，C++ 回 404。
    // catchall 只在别的路由都没匹配上时才触发，正好是单页应用兜底的语义。
    CROW_CATCHALL_ROUTE(app)
    ([fill_webapp](const crow::request& req, crow::response& res) {
        if (!webapp_owns(req.url)) {
            // 接口路径没匹配上就是真的没有这个接口，照常回 404 JSON
            res.code = 404;
            res.body = R"({"detail":"Not Found"})";
            res.set_header("Content-Type", "application/json");
            res.end();
            return;
        }
        fill_webapp(req, res);
        res.end();
    });

    // ---- 项目的新建、删除、改梗概 ----
    //
    // 同样是阶段 3 漏掉的。删项目那个不可逆，三道闸在 projects.cpp 里。

    CROW_ROUTE(app, "/api/new").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] {
                return post_new_project(parse_body(req.body),
                                        config::runtime().snapshot());
            });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/project/delete").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] {
                return post_delete_project(parse_body(req.body),
                                           config::runtime().snapshot());
            });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/project/premise").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] {
                return post_project_premise(parse_body(req.body));
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
                return post_episode(parse_body(req.body));
            });
            return json_response(r.body, r.status);
        });

    CROW_ROUTE(app, "/api/episode/action").methods("POST"_method)(
        [](const crow::request& req) {
            auto r = guard([&] {
                return post_episode_action(parse_body(req.body));
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
    static std::shared_ptr<llm::Client> batch_client =
        llm::make_client(llm::default_http_post());

    const auto batch_route = [](auto handler) {
        return [handler](const crow::request& req) {
            auto r = guard([&] {
                return handler(parse_body(req.body),
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
            // skip_draft 默认真，和 POST /api/run 一致。
            // **预览和实际必须是同一套默认**，否则它说的是另一件事。
            const char* sd = req.url_params.get("skip_draft");
            const bool skip_draft = sd == nullptr || std::string(sd) != "false";
            return get_run_preview(required_query(req, "path"),
                                   query(req, "episode_id"),
                                   query_bool(req, "all_episodes"),
                                   query_bool(req, "skip_final"), skip_draft,
                                   query_bool(req, "force"),
                                   config::runtime().profile());
        });
        return json_response(r.body, r.status);
    });

    // 服务端有哪些参考音色。角色页打开时顺带拉一次。
    //
    CROW_ROUTE(app, "/api/voices")([](const crow::request& req) {
        auto r = guard([&] {
            return get_voices(required_query(req, "path"),
                              config::runtime().snapshot().tts.backend);
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
                return post_run(parse_body(req.body),
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
    //
    // **两个地址是同一条路**，见 kWsRoutes 里为什么。
    const auto on_open = [](crow::websocket::connection& conn) {
        ws::hub().add(&conn);
        conn.send_text(json{{"type", "hello"},
                            {"service", "changji"}}.dump());
    };
    // 形参个数跟 Crow 版本走：1.2.0 是 (connection&, reason)，更新的版本
    // 多一个 uint16_t 关闭码。升级 Crow 时这里会编译报错，那是好事——
    // 静默的签名不匹配会让 onclose 根本不被调用，连接泄漏在注册表里，
    // 直到某次广播向已析构的对象发送才崩。
    const auto on_close = [](crow::websocket::connection& conn,
                             const std::string& /*reason*/) {
        ws::hub().remove(&conn);
    };
    const auto on_message = [](crow::websocket::connection& conn,
                               const std::string& data, bool is_binary) {
        if (is_binary) return;  // 上行没有二进制消息，忽略
        ws::hub().handle_client_message(&conn, data);
    };

    CROW_WEBSOCKET_ROUTE(app, "/ws")
        .onopen(on_open)
        .onclose(on_close)
        .onmessage(on_message);

    CROW_WEBSOCKET_ROUTE(app, "/api/ws")
        .onopen(on_open)
        .onclose(on_close)
        .onmessage(on_message);

    CROW_LOG_INFO << "changji 监听 " << opts.host << ":" << opts.port;

    app.bindaddr(opts.host)
        .port(static_cast<std::uint16_t>(opts.port))
        .concurrency(opts.concurrency)
        .run();
}

}  // namespace changji::http
