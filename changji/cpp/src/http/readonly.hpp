#pragma once

// 阶段 2 的只读接口。
//
// 六个接口：/api/hardware /api/project /api/shots /api/assets
//           /api/settings /api/projects
//
// 移植自 src/changji/web/server.py 里对应的路由。
//
// 契约标准是**结构兼容**（字段名、嵌套结构、取值、状态码一致，key 顺序不管），
// 见方案第三节。错误响应要和 FastAPI 的 HTTPException 一致：
// 状态码加一个 {"detail": "..."} 的 body——前端只认这个形状。

#include <string>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"

namespace changji::http {

/// 接口处理的结果：状态码加 JSON body。
///
/// 不直接返回 crow::response 是为了让单元测试能不起服务就把这些逻辑跑一遍。
/// 阶段 2 的对拍要比对的是 body，不是 HTTP 传输本身。
struct ApiResult {
    int status = 200;
    nlohmann::json body;
};

/// 抛出它等价于 FastAPI 的 raise HTTPException(status, detail)。
class ApiError : public std::runtime_error {
public:
    ApiError(int status, const std::string& detail)
        : std::runtime_error(detail), status_(status) {}
    int status() const { return status_; }

private:
    int status_;
};

// 每个接口一个纯函数。参数就是 Python 那边的查询参数。

ApiResult get_hardware(const config::Settings& settings);
ApiResult get_settings(const config::Settings& settings);
ApiResult get_projects(const config::Settings& settings);
ApiResult get_project(const std::string& path);
ApiResult get_shots(const std::string& path, const std::string& episode_id);
ApiResult get_assets(const std::string& path);

/// 把上面那些函数的异常翻成 ApiResult。路由层统一走它。
template <typename F>
ApiResult guard(F&& fn) {
    try {
        return fn();
    } catch (const ApiError& e) {
        return {e.status(), {{"detail", e.what()}}};
    } catch (const std::exception& e) {
        // Python 那边未捕获的异常会被 FastAPI 变成 500 加一句
        // "Internal Server Error"。这里把真实信息带出来——
        // 那句话什么也没说，用户报障时只能贴一张没用的截图。
        return {500, {{"detail", std::string("服务端出错：") + e.what()}}};
    }
}

}  // namespace changji::http
