#pragma once

// 工作流加载。
//
// 命令行和界面共用一份逻辑，避免两处各写一遍然后其中一处漏掉某个工作流。
// Python 那边就漏过：配音工作流做好了但没有任何地方去加载它。
//
// **查找顺序是先项目后内置。** 项目的 workflows/ 里放同名文件就能覆盖内置的，
// 这样不同的剧可以用不同的模型，不用改代码也不用改配置。
//
// 内置的那几份是**编进二进制**的，不是随程序装的数据文件——
// "exe 拷过去了、workflows 目录忘了拷"这种事的表现是跑到出片那一步才报
// "找不到工作流"，而前面几十分钟的配音和首帧已经跑完了。
//
// 移植自 src/changji/workflows_loader.py。

#include <map>
#include <optional>
#include <string>

#include "comfy/client.hpp"
#include "comfy/workflow.hpp"
#include "models/project.hpp"

namespace changji::comfy {

/// 内置工作流的原文。没有这个名字返回空。
std::optional<std::string> bundled_workflow(const std::string& name);

/// 加载一个工作流。
///
/// name 是不带扩展名的文件名，比如 video、tts、image。
/// 非必需的工作流找不到就返回空，由调用方决定退回什么行为。
///
/// 界面版需要服务端的节点定义才能转成接口版，所以要传 client；
/// 已经是接口版的直接包一层，不碰服务端。
std::optional<ApiWorkflow> load_workflow(Client& client,
                                         const models::ProjectStore& store,
                                         const std::string& name,
                                         bool required = true);

/// 加载全部工作流。
///
/// **只有视频是必需的**，其余缺了各有退路：没有图像工作流就用视频模型
/// 出单帧，没有配音工作流就用估算后端只算时长。
std::map<std::string, std::optional<ApiWorkflow>> load_all(
    Client& client, const models::ProjectStore& store);

}  // namespace changji::comfy
