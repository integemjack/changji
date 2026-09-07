#pragma once

// 把 ComfyUI 接成 frames / render 两个阶段的后端。
//
// 这一层的形状是**函数对象**，和 sd.cpp 那条路完全一样
// （stages::FrameRenderer / stages::VideoRenderer）。所以"在两个后端之间切"
// 就是换一个函数对象，流水线本身一行都不用改——那正是当初把后端抽成
// 回调而不是虚基类的理由。
//
// 移植自 src/changji/stages/render.py 的 RenderStage 和
// stages/frames.py 的 ImageModelFrameBackend。
//
// ---
//
// **改工作流参数这件事有一个反复出现的坑：不能按节点 id 猜。**
// 工作流里有两个 CLIPTextEncode（正、负），id 的大小只是画布上的创建顺序，
// 和它接到采样器的哪个输入槽没有关系。猜错的后果是正负提示词对调——
// 画面里出现的全是负面词里写的东西，而且不会报任何错。

#include <memory>
#include <string>

#include "comfy/client.hpp"
#include "comfy/workflow.hpp"
#include "models/project.hpp"
#include "stages/frames.hpp"
#include "stages/render.hpp"

namespace changji::comfy {

/// 正负提示词节点的 id。
///
/// **靠它们连到 KSampler 的哪个输入槽区分。** 找不到就抛——
/// 与其猜一个，不如让用户知道这个工作流的结构不认识。
std::pair<std::string, std::string> text_node_ids(const ApiWorkflow& w);

/// 设置画面尺寸。尺寸节点的类型因模型而异，逐个试。
///
/// 一个都没试中返回 false，由调用方决定是报错还是就这么跑
/// （有些工作流的尺寸是写死的，那也能出片，只是不听档位的）。
bool set_size(ApiWorkflow& w, int width, int height);

/// 用 ComfyUI 的图像工作流出首帧。
///
/// 这条路能吃角色定妆图和场景空景图做参考，跨镜头一致性远好于视频模型。
/// 工作流由用户提供，放在项目的 workflows/image.json。
///
/// 客户端用 shared_ptr、路径按值传：返回的函数对象会被存进 pipeline::Backends
/// 然后在工作线程上跑几十分钟，比这次调用活得久得多。传引用的话，
/// 调用方一不小心让它先析构，表现是工作线程访问已释放对象——
/// 而那时候栈上已经没有任何线索指向这里了。
stages::FrameRenderer image_frame_renderer(std::shared_ptr<Client> client,
                                           ApiWorkflow workflow,
                                           models::ProjectPaths paths);

/// 用 ComfyUI 的视频工作流出片。
///
/// 不需要 ProjectPaths：首帧是调用方按绝对路径给的，产出直接下载到
/// 指定的 dest。多接一个参数只会让人以为这一层还会自己去拼路径。
stages::VideoRenderer video_renderer(std::shared_ptr<Client> client,
                                     ApiWorkflow workflow);

}  // namespace changji::comfy
