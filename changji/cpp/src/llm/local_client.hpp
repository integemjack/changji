#pragma once

#include <memory>
#include <thread>

#include "config/settings.hpp"
#include "llm/client.hpp"
#include "models/hardware.hpp"

namespace changji::llm {

/// 进程内跑大模型。和 `RemoteClient` 同一个接口，换掉它就行。
///
/// **和远端那条的差别不只是"不用起服务"**：这一条归调度器管。
/// 借槽（`Slot::LLM`）才拿得到模型，出图出片要显存时调度器会按
/// **实时空闲显存**决定要不要把它卸掉——够就不动，一次重装是几十秒。
///
/// 权重路径在 `[models].llm`，`[llm].backend = "local"` 打开。
class LocalClient : public Client {
public:
    LocalClient() = default;
    std::string complete(const Request& req,
                         pipeline::CancelToken& tok) override;
    /// 边生边给。进程内这条路能逐 token 拿到，所以真流式的就是它。
    std::string complete(const Request& req, pipeline::CancelToken& tok,
                         const OnToken& on_token) override;
};

/// 进程内那条现在是什么状态。设置页的引擎卡读它。
///
/// **并发度是算不出来的，只能问。** 配置里 `[llm].parallel` 是个上限，
/// 实际开出来几个上下文由显存说了算（见 LlamaChat::load）。界面上不把
/// 这两个数分开摆，用户会以为自己配了 4 就是 4 路，而实际可能只有 1 路。
struct LocalLlmStatus {
    bool loaded = false;
    int slots = 0;           ///< 实际开出来的上下文数，也就是能同时跑几路
    int context_tokens = 0;  ///< 每个上下文多长
};

LocalLlmStatus local_llm_status();

/// 把大模型注册成调度器的一个槽。
///
/// **`[llm].backend != "local"` 时什么都不做**：远端那条没有本地权重，
/// 注册一个装不上的槽只会在借它的时候抛一句没意义的错。
///
/// 注册之后它是常驻的（`Residency::Cached`）：装上就不主动卸，显存真不够
/// 时才被驱逐。驱逐优先级设得比出图出片低，腾地方时先卸它——写剧本一集
/// 只跑一次，而出图出片每镜都要。
///
/// **注册不等于加载。** 调度器是借出时才装的——用户定的
/// 「用的时候才加载，不做启动预载」。
void register_llm_slot(std::function<config::Settings()> provider,
                       const models::HardwareProfile& profile);


/// 按 `[llm].backend` 造一个客户端。
///
/// **选一次就够，别每个请求选。** 把"走哪条后端"散到各个路由里的话，
/// 将来加第三条就要改三处；而且 local 那条每次都要重新借槽。
///
/// 配的是 local 但这个二进制没编进 llama.cpp 时**退回远端**并在 stderr 上
/// 说一声——比直接抛好：用户多半只是拿了个不带 llama 的构建，
/// 而远端那条只要地址填了就能用。
/// `post` 是远端那条要用的发送函数，由调用方注入（生产里传
/// `default_http_post()`）。**做成参数而不是在这里直接调**：
/// 那个函数只链进主目标，测试目标里没有，写死会让测试链不过。
std::shared_ptr<Client> make_client(HttpPost post);

}  // namespace changji::llm
