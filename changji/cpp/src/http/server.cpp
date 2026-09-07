#include "http/server.hpp"

#include <crow.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>

#include "doctor/doctor.hpp"
#include "http/editing.hpp"
#include "http/media.hpp"
#include "http/upload.hpp"
#include "http/readonly.hpp"
#include "http/planning.hpp"
#include "http/scripting.hpp"
#include "llm/client.hpp"
#include "http/ws.hpp"
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
    res.set_header("Content-Type", "application/json; charset=utf-8");
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

/// 取查询参数。Crow 拿不到时返回 nullptr，转成空串——
/// 空串该怎么处理由各个接口自己决定（比如 path 为空是 400 不是 500）。
std::string query(const crow::request& req, const char* key) {
    const char* v = req.url_params.get(key);
    return v ? std::string(v) : std::string();
}

}  // namespace

void run(const config::Settings& settings, const Options& opts) {
    crow::SimpleApp app;

    // job 表往 WebSocket 推消息，但它不认识 WebSocket——中间靠这个回调接上。
    // 分层的好处很实在：jobs.cpp 因此不用链 Crow，单元测试才编得动。
    pipeline::jobs().set_sink(
        [](const std::string& job_id, const json& msg) {
            ws::hub().broadcast(job_id, msg);
        });

    // ---- REST ----

    CROW_ROUTE(app, "/api/health")([] {
        return json_response({{"ok", true}, {"service", "changji"}});
    });

    CROW_ROUTE(app, "/api/doctor")([&settings] {
        // 体检里有三项要发网络请求，最坏情况阻塞二十多秒。
        // Crow 是线程池模型，这只占住一个工作线程，不影响其它请求。
        return json_response(to_json(doctor::run_checks(settings)));
    });

    // ---- 阶段 2：只读接口 ----
    //
    // 处理逻辑放在 readonly.cpp 里的纯函数，这里只负责取查询参数和转响应。
    // 那些函数不碰 crow 类型，单元测试能不起服务就把它们跑一遍。

    CROW_ROUTE(app, "/api/hardware")([&settings] {
        auto r = guard([&] { return get_hardware(settings); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/settings")([&settings] {
        auto r = guard([&] { return get_settings(settings); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/projects")([&settings] {
        auto r = guard([&] { return get_projects(settings); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/project")([](const crow::request& req) {
        auto r = guard([&] { return get_project(query(req, "path")); });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/shots")([](const crow::request& req) {
        auto r = guard([&] {
            return get_shots(query(req, "path"), query(req, "episode_id"));
        });
        return json_response(r.body, r.status);
    });

    CROW_ROUTE(app, "/api/assets")([](const crow::request& req) {
        auto r = guard([&] { return get_assets(query(req, "path")); });
        return json_response(r.body, r.status);
    });

    // ---- /api/media：带 Range 的文件服务 ----
    //
    // Range 是必须的，不是锦上添花：不实现的话前端 <video> 标签
    // 拖不动进度条，只能从头播。方案第三节点了名。

    CROW_ROUTE(app, "/api/media")([](const crow::request& req) {
        const auto t = resolve_media(query(req, "path"), query(req, "rel"));
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
        if (!r.has_value()) {
            // 语法不认识或起点越界。416 要带上 Content-Range 告诉对方真实长度。
            crow::response res(416);
            res.set_header("Content-Range", "bytes */" + std::to_string(size));
            res.set_header("Accept-Ranges", "bytes");
            return res;
        }

        const std::uint64_t len = r->last - r->first + 1;
        std::string chunk(static_cast<std::size_t>(len), '\0');
        in.seekg(static_cast<std::streamoff>(r->first));
        in.read(chunk.data(), static_cast<std::streamsize>(len));
        chunk.resize(static_cast<std::size_t>(in.gcount()));

        crow::response res(206, std::move(chunk));
        res.set_header("Content-Type", ctype);
        res.set_header("Accept-Ranges", "bytes");
        res.set_header("Content-Range",
                       "bytes " + std::to_string(r->first) + "-" +
                           std::to_string(r->last) + "/" + std::to_string(size));
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
    static llm::RemoteClient script_client(settings.llm, llm::default_http_post());

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

    // ---- 任务状态 ----
    //
    // POST /api/run 要等阶段 5 的流水线，这里先只挂查询和停止。
    // 这两个本身就是完整的：前端的进度轮询和停止按钮现在就能用。

    CROW_ROUTE(app, "/api/run")([] {
        return json_response(pipeline::jobs().snapshot(pipeline::JobKind::Run));
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
