#pragma once

// 大模型客户端。
//
// 两种后端长期共存，不是过渡状态：
//
//   Remote  —— 远端兼容 OpenAI 接口的服务（Ollama、云端）。对应 [llm] 配置段。
//   Local   —— 进程内 llama.cpp。对应 [models].llm。
//
// 决策 4 之后"用远端"不再是临时方案：树莓派没有跑 14B 的内存，
// 它只能打到局域网的 Windows 机器或者云端。所以这两条路都要留着。
//
// 还有第三个后端 Replay，只给测试和"回放模式"用——把录好的返回原样吐出来。
// 阶段 4 的完成判据里"回放模式下生成结果与 Python 一致"靠的就是它。
//
// ---
//
// HTTP 是**注入**的，这个文件不 include httplib。
// 一是分层，二是很实际：请求怎么拼、错误怎么翻成人话、返回怎么抽内容，
// 这三件事全是纯逻辑，能测死；混进真实网络之后就只能靠手工验了。

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "pipeline/jobs.hpp"

namespace changji::llm {

/// 调不通大模型时抛这个。消息是给用户看的，不是给开发看的。
class LlmError : public std::runtime_error {
public:
    explicit LlmError(const std::string& what) : std::runtime_error(what) {}
};

/// 一次请求。
struct Request {
    std::string prompt;
    /// 约束输出结构的 JSON Schema。空表示不约束。
    nlohmann::ordered_json schema;
    /// schema 的名字，走 response_format 时要填。
    std::string schema_name;
    double temperature = 0.7;
};

/// HTTP 响应。刻意只留用得上的三样。
struct HttpResponse {
    int status = 0;
    std::string body;
    /// 传输层就失败了（连不上、超时），这时 status 是 0。
    std::optional<std::string> transport_error;
};

/// 发一次 POST。由调用方注入。
using HttpPost = std::function<HttpResponse(
    const std::string& url, const std::string& body,
    const std::map<std::string, std::string>& headers, double timeout_s)>;

/// 客户端接口。
class Client {
public:
    virtual ~Client() = default;

    /// 同步生成，返回模型吐的原始文本。
    ///
    /// 取消令牌要在耗时点上查。远端后端能查的只有请求前后两个点——
    /// httplib 的同步调用中途打不断；进程内后端可以在每个 token 上查。
    virtual std::string complete(const Request& req,
                                 pipeline::CancelToken& tok) = 0;
};

/// 拼请求体。对应 Python 三个阶段里那份 payload。
///
/// 三个阶段的 payload 是同一个形状，Python 那边抄了三遍。这里合成一处——
/// 这不算破契约：契约是"发给大模型服务的东西"，形状一致就行。
nlohmann::ordered_json build_payload(const config::LLMConfig& cfg,
                                     const Request& req,
                                     bool json_schema_mode);

/// 从返回里抽出内容。抽不到抛 LlmError。
///
/// 对应 Python 的 body["choices"][0]["message"]["content"]，
/// 那边 KeyError/IndexError 都翻成"大模型返回格式异常"。
std::string extract_content(const std::string& raw_body);

/// 把 HTTP 状态码翻成一句能照着做的话。
///
/// 对应 Python 的 stages/_llm.py explain()。那个模块存在的理由是：
/// 三个阶段本来都是 raise_for_status() 了事，抛出的 httpx 异常一路穿到
/// 最外面变成一句「Internal Server Error」。而这恰恰是最常见的一类失败——
/// 地址填错了、模型名写错了、密钥过期了。用户该看到的是"去设置里改地址"。
std::string explain_status(const config::LLMConfig& cfg, int status,
                           const std::string& body);

/// 每次调用时取一份当前配置。
///
/// **不能在构造时拷一份。** /api/connections 能在运行期换大模型地址，
/// 拷一份的话改完之后剧本接口还在往老地址发，而界面已经显示"已应用"了。
using ConfigProvider = std::function<config::LLMConfig()>;

/// 远端 OpenAI 兼容服务。
class RemoteClient : public Client {
public:
    explicit RemoteClient(ConfigProvider cfg, HttpPost post);
    /// 配置固定不变的版本。测试用，生产代码应该传 provider。
    RemoteClient(config::LLMConfig cfg, HttpPost post);

    std::string complete(const Request& req, pipeline::CancelToken& tok) override;

private:
    ConfigProvider cfg_;
    HttpPost post_;
};

/// 回放。按调用顺序吐出预先录好的返回。
///
/// 给测试和阶段 4 的"回放模式"用：同一段提示词，Python 和 C++ 各跑一遍
/// 解析，比对结果。真调模型的话每次输出都不一样，什么都比不了。
class ReplayClient : public Client {
public:
    explicit ReplayClient(std::vector<std::string> responses);
    std::string complete(const Request& req, pipeline::CancelToken& tok) override;

    /// 录下的每一次请求。测试要拿它检查提示词拼对没有。
    const std::vector<Request>& calls() const { return calls_; }

private:
    std::vector<std::string> responses_;
    std::vector<Request> calls_;
    std::size_t next_ = 0;
};

/// 用 cpp-httplib 发请求。定义在 client_http.cpp 里，那个文件才 include httplib。
HttpPost default_http_post();

}  // namespace changji::llm
