#pragma once

#include <filesystem>

#include "config/settings.hpp"

namespace changji::infer {

/// 把一段视频逐帧过 ESRGAN 超分，再按目标尺寸压回去。
///
/// **为什么要有它：** MiniMax-H3 在 32 GB 的卡上只能出 960×544（1280×704
/// 装不下）。超分是不换硬件把分辨率拉上去的那条路——ESRGAN 模型只有几十兆，
/// 和扩散模型没关系，跑在出好的帧上。
///
/// **已知的风险，用之前要看片子确认：** 超分是逐帧独立做的，没有帧间一致性，
/// 细密纹理（雨丝、布料、玻璃上的水痕）容易在帧之间闪。三秒的镜头里可能
/// 还能忍，但这要看了才知道，不能只看单帧。
///
/// 做法是先按模型自己的倍率放大（x4plus 就是 4 倍），再用 ffmpeg 的 lanczos
/// 压到 `out_w × out_h`。**先放大再压回去比直接放 2 倍好**：多出来的细节在
/// 降采样时会被平均掉一部分，噪点和锯齿也一起平掉了。
///
/// `model` 是 ESRGAN 权重（RealESRGAN_x4plus.pth 这类）。
/// 出错抛 `SdError`。
void upscale_video(const std::filesystem::path& in,
                   const std::filesystem::path& out,
                   const std::filesystem::path& model, int out_w, int out_h,
                   const config::AssemblyConfig& assembly);

}  // namespace changji::infer
