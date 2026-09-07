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
#include "models/hardware.hpp"

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
///
/// detail 可以是字符串，也可以是任意 JSON。后者是为了对上 FastAPI 自己的
/// 校验错误：pydantic 的 extra="forbid" 之类违规回的是 **422** 而不是 400，
/// 而且 detail 是一个结构化数组
/// （[{type, loc, msg, input}]），不是一句话。对拍语料抓到过这一条。
class ApiError : public std::runtime_error {
public:
    ApiError(int status, const std::string& detail)
        : std::runtime_error(detail), status_(status), detail_(detail) {}

    /// 带完整 JSON body 的那种。**不做成构造函数重载**：
    /// const char* 到 std::string 和到 nlohmann::json 都是一次用户定义转换，
    /// 两个构造函数会让 ApiError(400, "字面量") 变成歧义调用。
    static ApiError with_body(int status, const nlohmann::json& body) {
        ApiError e(status, body.dump());
        e.detail_ = body;
        return e;
    }

    int status() const { return status_; }
    const nlohmann::json& detail() const { return detail_; }

private:
    int status_;
    nlohmann::json detail_;
};

/// FastAPI 在 pydantic 校验失败时回的那种 422，**字段嵌在子模型里**的情形。
///
/// loc 的头两段固定是 ["body", <模型字段名>]，第三段是出问题的键。
/// 字段直接挂在请求模型上时 loc 只有两段，用下面的 unprocessable_top。
/// 形状对不上的话前端拿到的 detail 是数组而不是字符串，
/// 错误提示会显示成 [object Object]——那是现有行为，要原样保住。
inline ApiError unprocessable(const std::string& model_field,
                              const std::string& key,
                              const std::string& msg,
                              const nlohmann::json& input,
                              const std::string& type) {
    return ApiError::with_body(422, nlohmann::json{{"detail", nlohmann::json::array({
        nlohmann::json{
            {"type", type},
            {"loc", nlohmann::json::array({"body", model_field, key})},
            {"msg", msg},
            {"input", input},
        }})}});
}

/// 同上，但字段**直接挂在请求模型上**，loc 只有 ["body", <键>] 两段。
///
/// 两个函数不能合并：段数是 pydantic 按模型嵌套层数生成的，
/// 多一段少一段前端高亮的就是别的字段。
inline ApiError unprocessable_top(const std::string& key,
                                  const std::string& msg,
                                  const nlohmann::json& input,
                                  const std::string& type) {
    return ApiError::with_body(422, nlohmann::json{{"detail", nlohmann::json::array({
        nlohmann::json{
            {"type", type},
            {"loc", nlohmann::json::array({"body", key})},
            {"msg", msg},
            {"input", input},
        }})}});
}

// 每个接口一个纯函数。参数就是 Python 那边的查询参数。

ApiResult get_hardware(const config::Settings& settings);
ApiResult get_settings(const config::Settings& settings);

/// 同上，但**画像由调用方给**。
///
/// 路由层要传 runtime().profile()——那份应用了 /api/settings 改过的
/// 画质档位。上面那两个自己 detect() 一遍，会把覆盖绕过去，
/// 表现是用户改了草稿分辨率，接口回"已应用"，但 /api/hardware 还是老值。
/// 这个坑实机验证时撞到过：单元测试直接调 runtime().profile() 所以没发现。
ApiResult get_hardware(const config::Settings& settings,
                       const models::HardwareProfile& profile);
ApiResult get_settings(const config::Settings& settings,
                       const models::HardwareProfile& profile);
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
        // detail 已经是完整 body（422 那种）就原样返回，
        // 否则包成 {"detail": "..."}——HTTPException 的形状。
        const auto& d = e.detail();
        if (d.is_object() && d.contains("detail")) return {e.status(), d};
        return {e.status(), {{"detail", d}}};
    } catch (const std::exception& e) {
        // Python 那边未捕获的异常会被 FastAPI 变成 500 加一句
        // "Internal Server Error"。这里把真实信息带出来——
        // 那句话什么也没说，用户报障时只能贴一张没用的截图。
        return {500, {{"detail", std::string("服务端出错：") + e.what()}}};
    }
}

}  // namespace changji::http
