#pragma once

// 首次运行的初始化页要的四条接口。
//
// **为什么在 /bff 不在 /api。** `/api/*` 整套在和 Python 的对拍覆盖范围内，
// 而下模型这件事 Python 侧压根没有——那边模型是 ComfyUI 自己管的。
// 加进 /api 就是一处破契约，而这一条完全没必要破。
//
// ---
//
// **为什么要有这一页。** 装好程序打开界面，用户看到的是一个能点的项目页，
// 点到「出片」才发现什么都跑不了，报的是"本地模型一个都没配"。那时候他
// 手上有的只有一句提示和一个配置文件路径，要自己去翻部署手册、找镜像地址、
// 挑量化档、算显存装不装得下。这一页把那段路变成"看一眼推荐、点下载、等"。
//
// **进不去首页是有意的**（只在必需的那几组缺东西时）。可以跳过，
// 但默认拦一下——一个什么都跑不了的首页不比这一页有用。

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "config/settings.hpp"
#include "http/readonly.hpp"
#include "models/hardware.hpp"
#include "setup/catalog.hpp"

namespace changji::http {

/// GET /bff/setup/state —— 有哪些模型、这台机器该选哪个、盘上已经有什么。
ApiResult get_setup_state(const config::Settings& settings,
                          const models::HardwareProfile& profile);

/// POST /bff/setup/download —— 开下。body:
///     {"selections": {"video": "h3-q4-turbo", …}, "dir": "可选，模型放哪"}
///
/// 立刻返回，进度去 /bff/setup/progress 拿。已经在下的时候回 409——
/// **不是静默忽略**：用户点了第二次而界面什么都没变的话，
/// 他会以为第一次没点上。
ApiResult post_setup_download(const config::Settings& settings,
                              const nlohmann::json& body);

/// GET /bff/setup/progress —— 下到哪儿了。
ApiResult get_setup_progress();

/// POST /bff/setup/cancel —— 停下。已经下好的留着，下次接着下。
ApiResult post_setup_cancel();

/// 这一组配齐了没有。
///
/// 编剧和配音有另一条出路：接外面的服务。那时候本机一个模型文件都没有
/// 也算配齐了——**不认这一条的话，用云端大模型的人会被永远挡在这一页上**。
/// 这个模型文件还在下载中途吗（旁边有没有 aria2 的控制文件）。
///
/// **光比大小是判不出来的**：aria2c 一开始就把文件按最终大小整个预分配好，
/// 再往里填。所以下到 10% 的时候 `file_size` 返回的正好是清单里那个数，
/// "大小对上了 = 下完了"这条就被骗过去。2026-09-10 真被坑到：61.7 GB 的
/// bf16 才下了 6 GB，初始化页显示已完成，用户选了它，出片直接花屏——
/// 而且**没有任何报错**，safetensors 的头在文件开头早下下来了，解析得
/// 好好的，错的是后面那些还是零的张量。
bool download_in_progress(const std::filesystem::path& model_file);

bool group_satisfied(const setup::Group& g, const config::Settings& settings);

/// 把 `config_patch` 那种 patch 应用到内存里的配置。
///
/// **单独一个函数是因为不能重新读盘。** 重读会丢掉项目那一层的覆盖
/// （画幅写在项目的 changji.toml 里），而这个请求不知道当前是哪个项目。
/// 只认这一页会写的那些键，别的忽略。
void apply_setup_patch(config::Settings& settings, const nlohmann::json& patch);

/// `[models]` 里那个角色对应的字段。角色名不认识返回 nullptr。
///
/// 导出是为了能测：漏一个角色的表现是"下完了但配置里没写上"，
/// 而那要到出片时才报"模型没配"。
std::string* models_field(config::ModelsConfig& m, const std::string& role);

}  // namespace changji::http
