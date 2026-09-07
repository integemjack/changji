#pragma once

// ComfyUI 客户端。
//
// 用 WebSocket 监听进度而不是轮询历史接口，因为成片档一个镜头要跑好几分钟，
// 轮询既慢又容易漏掉中间状态。
//
// **失败分两类，处理方式完全不同：**
//
//   提交时的校验失败（模型文件缺失、参数越界）是确定性的，重试没有意义，直接抛。
//   执行中的失败（显存不足、节点崩溃）可能是偶发的，值得重试。
//
// 把两类混在一起重试，最常见的表现是"模型文件名打错一个字母，界面上转了
// 四分钟才报错"——因为它把一个必然失败的请求重试了三次，每次都等到超时。
//
// 移植自 src/changji/comfy/client.py。
//
// ---
//
// 传输是**注入**的，这个文件不 include httplib、也不 include 任何 WebSocket 库。
// 理由和 llm/client.hpp 一样：请求怎么拼、错误怎么分类、消息流怎么解释，
// 全是纯逻辑，能测死；混进真实网络之后就只能靠手工验了。
// 而这一层最容易错的地方恰恰是"什么时候算跑完"——那要靠喂一串脚本消息才测得出。

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "comfy/workflow.hpp"
#include "config/settings.hpp"
#include "pipeline/jobs.hpp"

namespace changji::comfy {

/// ComfyUI 相关错误的基类。
class ComfyError : public std::runtime_error {
public:
    explicit ComfyError(const std::string& what) : std::runtime_error(what) {}
};

/// 连不上。地址错了、服务没起、或者网络不通。
class ComfyUnavailable : public ComfyError {
public:
    explicit ComfyUnavailable(const std::string& what) : ComfyError(what) {}
};

/// 提交时被服务端拒绝。**重试没有意义。**
///
/// 最常见的原因是工作流引用的模型文件在服务端不存在。
class PromptValidationError : public ComfyError {
public:
    PromptValidationError(const std::string& what, OrderedJson node_errors)
        : ComfyError(what), node_errors_(std::move(node_errors)) {}

    const OrderedJson& node_errors() const { return node_errors_; }

    /// 把服务端的报错翻译成能看懂的话。
    ///
    /// 原样透传的话用户看到的是一坨嵌套 JSON，而里面真正有用的只有
    /// "哪个节点要的哪个文件服务端上没有"。
    std::string human_summary() const;

private:
    OrderedJson node_errors_;
};

/// 执行中失败。可能是偶发的，值得重试。
class ExecutionError : public ComfyError {
public:
    explicit ExecutionError(const std::string& what) : ComfyError(what) {}
};

/// 一次进度回报。
struct JobProgress {
    std::string prompt_id;
    std::string node_id;
    int step = 0;
    int total = 0;

    double fraction() const {
        return total ? static_cast<double>(step) / total : 0.0;
    }
};

using OnProgress = std::function<void(const JobProgress&)>;

/// 一个任务的结果。
struct JobResult {
    std::string prompt_id;
    /// 服务端 history 里的 outputs：节点 id -> {images: [...], ...}
    OrderedJson outputs = OrderedJson::object();
    double elapsed_s = 0.0;

    /// 取产出文件。**视频节点也把结果放在 images 键下**，带 animated 标记。
    /// 按 "videos" 找的话一个都找不到。
    std::vector<OrderedJson> files(const std::string& kind = "images") const;
    std::optional<OrderedJson> first_file() const;
};

/// HTTP 响应。和 llm 那边同一个形状。
struct HttpResponse {
    int status = 0;
    std::string body;
    /// 传输层就失败了（连不上、超时），这时 status 是 0。
    std::optional<std::string> transport_error;
};

/// 从 WebSocket 取下一条**文本**消息。
///
/// 返回 nullopt 表示这段时间内没有消息（等超时了），不是出错——
/// 调用方要拿它当"去查一次历史兜底"的信号。二进制帧（预览图）由实现自己吞掉。
///
/// 连接坏了抛异常，调用方退回轮询。
using WsRecv = std::function<std::optional<std::string>(double timeout_s)>;

/// 传输层。全部由调用方注入。
///
/// path 是相对 base_url 的（比如 "/prompt"），拼完整地址是传输层的事——
/// 这一层不该知道 base_url 长什么样，那样换个反向代理前缀就不用改这里。
struct Transport {
    std::function<HttpResponse(const std::string& path, double timeout_s)> get;
    std::function<HttpResponse(const std::string& path, const std::string& json_body,
                               double timeout_s)> post_json;
    /// 上传一个文件到 /upload/image。
    std::function<HttpResponse(const std::string& path,
                               const std::filesystem::path& file,
                               const std::string& subfolder, double timeout_s)> upload;
    /// 下载到本地文件。
    std::function<HttpResponse(const std::string& path,
                               const std::map<std::string, std::string>& query,
                               const std::filesystem::path& dest,
                               double timeout_s)> download;
    /// 连 WebSocket。**连不上返回空的 WsRecv**（不是抛），调用方退回轮询。
    std::function<WsRecv(const std::string& url)> connect_ws;
    /// 睡一会儿。注入是为了让重试退避在测试里不真的睡 1+2+4 秒。
    std::function<void(double seconds)> sleep;
};

/// 取配置的回调。**每次用的时候现取**，不是构造时存一份。
///
/// 这条是有代价学来的：LLM 客户端当初在构造时把 settings.llm 存了下来，
/// 用户在设置页改了地址、接口回"已保存"，而请求还是发往老地址。
using ConfigProvider = std::function<config::ComfyConfig()>;

class Client {
public:
    Client(ConfigProvider cfg, Transport transport, std::string client_id = "");

    /// 服务是否可用。不抛，连不上就是 false。
    bool ping();

    OrderedJson system_stats();

    /// 节点定义。格式转换和模型清单都要用，**缓存起来**——
    /// 一份 object_info 是几百 KB，每个镜头拉一次是纯浪费。
    const OrderedJson& object_info(bool refresh = false);

    WorkflowConverter converter();

    /// 查服务端上某个节点的某个下拉框有哪些可选值。
    ///
    /// 用来在提交之前就发现模型缺失，而不是等服务端拒绝——
    /// 提交之后才发现的话，报错要从 node_errors 里翻出来。
    std::vector<std::string> available_models(const std::string& node_class,
                                              const std::string& input_name);

    /// 提交任务，返回 prompt_id。校验失败抛 PromptValidationError。
    std::string submit(const ApiWorkflow& w, const std::string& client_id = "");

    /// 等一个任务跑完。走 WebSocket，掉线自动退回轮询。
    JobResult wait(const std::string& prompt_id, const OnProgress& on_progress,
                   double timeout_s, const std::string& client_id,
                   pipeline::CancelToken& tok);

    /// 提交并等待完成。执行类错误按配置重试，校验类错误直接抛。
    JobResult run(const ApiWorkflow& w, const OnProgress& on_progress,
                  pipeline::CancelToken& tok, double timeout_s = 0.0);

    /// 上传图片到服务端的 input 目录，返回可在工作流里引用的文件名。
    ///
    /// ComfyUI 可能在另一台机器上，所以**不能直接传本地路径**。
    std::string upload_image(const std::filesystem::path& p,
                             const std::string& subfolder = "");

    /// 把产出文件下载到本地。同样因为服务端可能在别的机器上。
    std::filesystem::path download(const OrderedJson& file_ref,
                                   const std::filesystem::path& dest);

    void interrupt();

    const std::string& client_id() const { return client_id_; }

private:
    /// 消费 WebSocket 消息流，直到任务跑完或者出错。
    JobResult consume(const WsRecv& recv, const std::string& prompt_id,
                      const OnProgress& on_progress, double deadline,
                      pipeline::CancelToken& tok);
    JobResult wait_poll(const std::string& prompt_id, double deadline,
                        pipeline::CancelToken& tok);
    /// 从历史里取产出。missing_ok 表示这是一次兜底查询：任务还没跑完就
    /// 返回 nullopt，让调用方接着等，而不是把空结果当成跑完了。
    std::optional<OrderedJson> history_outputs(const std::string& prompt_id,
                                               bool missing_ok);
    OrderedJson get_json(const std::string& path);

    ConfigProvider cfg_;
    Transport t_;
    std::string client_id_;
    OrderedJson object_info_;
    bool object_info_loaded_ = false;
};

/// 从 history 的 status 里抠出那句错误。
///
/// 单独暴露是因为它是**纯函数而且形状很刁**：messages 是
/// `[["execution_error", {...}], ...]` 这种数组套数组，取错一层就只剩
/// "任务执行失败"——那句话对排查没有任何帮助。
std::string error_from_history(const OrderedJson& status);

/// 随机的 client id。ComfyUI 按它记 WebSocket 订阅。
std::string random_client_id();

}  // namespace changji::comfy
