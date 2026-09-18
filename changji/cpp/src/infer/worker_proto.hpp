#pragma once

// 工作进程的协议：一次任务发什么、回什么。
//
// **为什么要有工作进程。** 见方案「多卡和多机怎么用起来」那一节。一句话：
// sd.cpp 的进度回调是全局的（`sd_set_progress_callback` 不带 ctx），
// 同进程里两个生成会互相串取消和进度——**同种模型的多实例只能靠多进程**。
// 上游接口形状改不了。
//
// 顺带白送一条：**崩溃隔离**。今天 `sd_image` 那个 0xc0000094 是整个服务
// 进程没了，跑了一半的一章、WebSocket 连接、排队的任务全丢。切成工作进程
// 之后，协调者只看到"这一镜的连接断了"，记一次失败接着跑。
// 这一条单卡起一个工作进程就有，不需要多卡。
//
// ---
//
// 序列化单独放一层是为了**能测**：接上真模型之后一个用例要跑几分钟，
// 协议层的 bug 就没法反复撞了。这和 `scheduler.hpp` 把加载卸载做成
// 注入回调是同一个理由。

#include <map>
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
///
/// **配音也在里头**，虽然它不吃显卡那么狠：那张「机器 × 能力」的表上
/// 配音是一列，能勾就得能派，否则界面在说一件做不到的事。
enum class TaskKind { Frame, Video, Tts };

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
    /// 尾帧图（首尾帧那条路，FL2VA）。2026-09-16 之前这一项不在协议里：
    /// 填了 last_frame_prompt、尾帧也出来了，走工作进程池就退回单帧图生
    /// 视频，一声不吭——同一章换条执行路径结果不同。
    std::optional<std::string> end_image;
    /// 留不留模型自己出的声音。**跟着任务走**，不读工作进程那台的设置：
    /// 那样同一章里不同节点出的镜头一半有环境声一半没有。没带就按本机。
    std::optional<bool> keep_ambient;
    /// 产物落在哪。**绝对路径**——工作进程可能在别的工作目录里跑。
    ///
    /// `return_artifact` 为真时这一项**没有意义**：那时候对面写的是自己的
    /// 沙箱，产物由派活方拉回来落到自己这边。
    std::string dest;

    /// 产物别写 dest，写你自己的沙箱，然后把内容指纹回给我。
    ///
    /// **跨机时必须为真**：dest 是派活方的路径，对面根本没有那个目录。
    /// 同机（本机多卡）留假——同一个文件系统，让它直接写省一次搬运。
    bool return_artifact = false;
    /// 随机种子。协调者算好传下来，**不能让工作进程自己算**：
    /// 它不知道 attempts，算出来的图和串行跑的会不一样。
    std::int64_t seed = 0;

    /// 这部电影挑的档位：{组: 选项 id}，来自项目的 `[models.pick]`。
    ///
    /// **带 id，不带路径。** 别的机器的模型目录在别处、盘符都可能不一样，
    /// 而"这部电影要 h3-full-q4_k_m"这件事是跟着电影走的、跟机器无关。
    /// 那台拿到之后照这个 id 去**自己的**模型目录里找文件
    /// （`setup::with_selections`）。
    ///
    /// 空 = 这部电影没挑过（老项目），那时候每台按自己 `[models]` 里写的
    /// 文件名跑，和以前一模一样。
    std::map<std::string, std::string> pick;

    // ---- 配音专用。别的 kind 忽略 ----

    /// 要念的那句话。
    std::string text;
    /// 用哪个音色。空 = 让那台自己挑。
    std::string voice_id;
    /// 情绪标签和强度，照 `Synthesizer` 的签名传下去。
    std::string emotion;
    double intensity = 0.0;
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
    ///
    /// **跨机时这是对面沙箱里的路径**，对派活方没用，看 `artifact_id`。
    std::string dest;

    /// 配音出来多长（秒）。**只有配音任务有意义。**
    ///
    /// 为什么要回这个数而不是让派活方自己去量：配音先行那条线靠它反推
    /// 镜头时长，而量一次要么读 wav 头要么起 ffprobe——活是在那台跑的，
    /// 它顺手就知道，没道理让派活方再算一遍。
    double duration_s = 0.0;

    /// 产物的内容指纹。`Task::return_artifact` 为真时才有。
    ///
    /// 派活方拿它去 `GET /blob/<id>` 把产物取回来。**产物和输入走的是
    /// 同一个 blob 仓库**——重跑一镜出来字节相同的话，第二次连传都不用传。
    std::string artifact_id;
};

nlohmann::json to_json(const TaskResult& r);
TaskResult task_result_from_json(const nlohmann::json& j);

/// 工作进程报的进度。
struct TaskProgress {
    /// queued / running / done / failed
    std::string state = "queued";
    int step = 0;
    int steps = 0;
    /// 这一下报的是哪个阶段：prep / sample / decode，见 infer::Phase。
    std::string phase = "sample";
    /// 采样中途最新的那张预览（`data:image/png;base64,…`）和它是第几步的。
    /// **只在派活方问的时候带**（`GET /task/<id>?preview_after=N`，且比 N
    /// 新）：一张几十 KB，跨境公网 25 KB/s，每次轮询都带会把链路吃光。
    /// 没有就是 -1 / 空。
    int preview_step = -1;
    std::string preview;
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
