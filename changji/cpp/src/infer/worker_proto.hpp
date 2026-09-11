#pragma once

// 工作进程的协议：一次任务发什么、回什么。
//
// **为什么要有工作进程。** 见方案「多卡和多机怎么用起来」那一节。一句话：
// sd.cpp 的进度回调是全局的（`sd_set_progress_callback` 不带 ctx），
// 同进程里两个生成会互相串取消和进度——**同种模型的多实例只能靠多进程**。
// 上游接口形状改不了。
//
// 顺带白送一条：**崩溃隔离**。今天 `sd_image` 那个 0xc0000094 是整个服务
// 进程没了，跑了一半的一集、WebSocket 连接、排队的任务全丢。切成工作进程
// 之后，协调者只看到"这一镜的连接断了"，记一次失败接着跑。
// 这一条单卡起一个工作进程就有，不需要多卡。
//
// ---
//
// 序列化单独放一层是为了**能测**：接上真模型之后一个用例要跑几分钟，
// 协议层的 bug 就没法反复撞了。这和 `scheduler.hpp` 把加载卸载做成
// 注入回调是同一个理由。

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "models/hardware.hpp"
#include "models/shot.hpp"
#include "stages/prompt_compose.hpp"
#include "stages/render.hpp"

namespace changji::infer {

/// 一次任务要干的活。
enum class TaskKind { Frame, Video };

const char* to_string(TaskKind k);
std::optional<TaskKind> task_kind_from(const std::string& s);

/// 派给工作进程的一次任务。
///
/// **输入是协调者算好的**：提示词已经拼完、档位已经算完、产物该落在哪
/// 也定了。工作进程不碰项目文件、不做判断、只算——
/// 这是"单一写者"那条线在进程边界上的体现。
struct Task {
    TaskKind kind = TaskKind::Frame;
    std::string shot_id;
    /// 出图用。出片时忽略。
    stages::PromptBundle prompts;
    models::TierSpec spec;
    /// 出片用。出图时忽略。
    int frames = 0;
    std::string motion;
    models::StyleLine style_line = models::StyleLine::REALISTIC;
    models::Tier tier = models::Tier::DRAFT;
    /// 首帧图。出片时可能有（图生视频），出图时没有。
    std::optional<std::string> start_image;
    /// 产物落在哪。**绝对路径**——工作进程可能在别的工作目录里跑。
    std::string dest;
    /// 随机种子。协调者算好传下来，**不能让工作进程自己算**：
    /// 它不知道 attempts，算出来的图和串行跑的会不一样。
    std::int64_t seed = 0;
};

nlohmann::json to_json(const Task& t);
/// 解析失败时抛 std::runtime_error，消息里说清缺什么。
Task task_from_json(const nlohmann::json& j);

/// 一次任务的结果。
struct TaskResult {
    bool ok = false;
    /// 失败时的原因。**要能直接给用户看**——它会变成事件流里那条 warn。
    std::string error;
    /// 产物的绝对路径。成功时才有意义。
    std::string dest;
};

nlohmann::json to_json(const TaskResult& r);
TaskResult task_result_from_json(const nlohmann::json& j);

/// 工作进程报的进度。
struct TaskProgress {
    /// queued / running / done / failed
    std::string state = "queued";
    int step = 0;
    int steps = 0;
    /// 这一下报的是加载权重还是采样。
    bool loading = false;
    std::optional<TaskResult> result;
};

nlohmann::json to_json(const TaskProgress& p);
TaskProgress task_progress_from_json(const nlohmann::json& j);

/// 工作进程拒了任务（回的不是 200/202）时给用户看的那句话。
///
/// body 只带前 200 个**字符**，而且按字符截、不按字节：这句话会变成
/// 事件流里那条 warn，进任务快照，再序列化成 JSON。按字节截落在半个汉字上，
/// nlohmann 就在序列化那一步抛 type_error.316，整个快照接口回 500，
/// 进度全看不见——json_extract 那一处 2026-09-11 实跑就是这么炸的。
///
/// 放在这一层而不是 worker_pool.cpp 里，是因为那个文件链 httplib、
/// 进不了测试目标；这两句话的形状在这儿能测。
std::string worker_rejected_message(int status, const std::string& body);

/// 工作进程回了 2xx 但 body 不是 `{"id":...}` 时的那句话。截法同上。
std::string worker_bad_accept_message(const std::string& body);

}  // namespace changji::infer
