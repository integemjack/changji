#pragma once

#include <memory>

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
};

/// 把大模型注册成调度器的一个槽。
///
/// **`[llm].backend != "local"` 时什么都不做**：远端那条没有本地权重，
/// 注册一个装不上的槽只会在借它的时候抛一句没意义的错。
///
/// 注册之后它默认是常驻的（`Residency::Cached`）——用户要的"默认加载
/// llm"。驱逐优先级设得比出图出片低，腾地方时先卸它：写剧本一集只跑一次，
/// 而出图出片每镜都要。
void register_llm_slot(std::function<config::Settings()> provider,
                       const models::HardwareProfile& profile);

}  // namespace changji::llm
