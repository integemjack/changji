#pragma once

// 正在干的那些**短活**。
//
// 任务表（jobs.hpp）管的是长跑任务：出片、写整季。它们有取消令牌、有事件
// 环、有快照接口，一种只能有一个。而引擎上还有另外一半 AI 活儿是**同步
// 请求**——生成一张参考图、写一章、改一段稿、朗读一段。它们几十秒就回，
// 不需要那一整套。
//
// 但顶栏那块「AI 作业中」要回答的是"引擎现在在忙什么"，而不是"长跑任务表
// 里有什么"。2026-09-11 实地撞上过一次：用户在生成参考图（占着图像槽），
// 另一头的批量写作四章全部失败在「显存不够加载 LLM：「图像」正用着」，
// 而那一刻界面上 `jobs: []`、顶栏一片安静、GPU 占用 0%——挡路的那件事
// 整个不可见，只能登服务器翻日志才查得到。
//
// 所以这里补一本**很轻的账**：谁在干活、干的哪个项目、干到哪儿了。
// 一个 RAII 对象，构造登记、析构划掉——出异常也不会留下幽灵记录。
//
// 刻意**不**做的事：没有取消（这些活本来就短）、没有事件环、没有排队。
// 它只回答一个问题，就是顶栏那一行字。

#include <cstdint>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

namespace changji::pipeline {

/// 一件正在干的短活。构造即登记，析构即划掉。
///
/// 用法就是往栈上一放：
/// ```
/// pipeline::Activity act{"image", project_path, "", "正在画参考图"};
/// ```
/// 中途想改那句话就 `act.set_message(...)`，有进度就 `act.set_progress(...)`。
class Activity {
public:
    /// `kind` 是界面上那个小标签认得的类型，见前端 JobBadge 里的 KIND 表。
    /// `project` 是项目目录的绝对路径（顶栏靠它点过去），留空就只显示名字。
    Activity(std::string kind, std::string project, std::string episode_id,
             std::string message);
    ~Activity();

    Activity(const Activity&) = delete;
    Activity& operator=(const Activity&) = delete;
    Activity(Activity&&) = delete;
    Activity& operator=(Activity&&) = delete;

    void set_message(std::string m);
    void set_progress(int current, int total);

    /// 盖在那句话上的一层：排队时写「排队中，前面还有 2 件」，轮到了就清掉。
    ///
    /// **盖一层而不是改那句话**，是因为排完了还得说回原来那句（"正在画参考
    /// 图"），而那句话是谁写的、写的什么，排队这一层不知道也不该知道。
    void set_note(std::string n);

private:
    std::uint64_t id_ = 0;
};

/// 这个线程此刻在干的那件活。没有就是 nullptr。
///
/// **给深处的代码用的。** 排队发生在调度器里（infer/scheduler），而那句
/// "排队中"要写到顶栏的账本上；一层层往下传一个回调的话，llm、sd、tts
/// 三条路每一条都要改签名，而它们和"界面上显示什么"没有半点关系。
Activity* current_activity();

/// 接到 Scheduler::AcquireOptions::on_queued 上：把排队情况写到当前线程
/// 那件活的那句话上。`ahead < 0` 表示不排了（轮到了或者不等了）。
void note_queued(int ahead, const std::string& blocker);

/// 此刻在干的那些短活，一行一个。形状和 JobTable::running_jobs() 一样，
/// 顶栏把两边接成一个列表。
nlohmann::json running_activities();

/// 引擎此刻在干的**全部** AI 活：长跑任务加短活，接成一个列表。
///
/// **只此一处拼。** 这份东西有两个出口（两秒一次推给浏览器的 system 消息，
/// 和 `/api/system` 那条给人 curl 的），两边各拼一次的话迟早只改一边——
/// 而症状是"界面上明明有任务，curl 出来却是空的"，查起来先怀疑的准是
/// 前端。已经栽过一次了。
nlohmann::json running_work();

}  // namespace changji::pipeline
