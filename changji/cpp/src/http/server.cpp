#include "http/server.hpp"

#include <crow.h>
#include <nlohmann/json.hpp>

#include "doctor/doctor.hpp"
#include "http/ws.hpp"

namespace changji::http {

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

}  // namespace

void run(const config::Settings& settings, const Options& opts) {
    crow::SimpleApp app;

    // ---- REST ----

    CROW_ROUTE(app, "/api/health")([] {
        return json_response({{"ok", true}, {"service", "changji"}});
    });

    CROW_ROUTE(app, "/api/doctor")([&settings] {
        // 体检里有三项要发网络请求，最坏情况阻塞二十多秒。
        // Crow 是线程池模型，这只占住一个工作线程，不影响其它请求。
        return json_response(to_json(doctor::run_checks(settings)));
    });

    // ---- WebSocket ----
    //
    // 阶段 0 只验证连接、订阅和广播这条链路是通的。
    // 真正的进度消息要等阶段 4 有了 job 表之后才有东西可推。

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
