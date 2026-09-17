#pragma once

// 引擎现在在干什么、待会儿要干什么、刚才干完了什么——**一本账，三个状态**。
//
// 用户 2026-09-17：「再增加任务页面显示正在做的（已经用时，结束图标按钮）、
// 排队中的（预计什么时候开始，取消图标按钮）、已经做完的（耗时），如果是
// 大模型有思考的还得显示思考点击展开思考内容」。
//
// 在这之前引擎里有两本半账，各答一半问题：
//
//   · `jobs.hpp` 的任务表：长跑任务（出片、写整季），一种一个槽，有取消
//     令牌、有事件流。答的是"那一条大任务跑到哪儿了"。
//   · `activity.hpp` 的短活：出一张参考图、写一章、朗读一段，构造即登记、
//     析构即划掉。答的是"引擎此刻在忙什么"。
//   · 半本是各处自己排的队（出图那一批、一集里的几十镜），**只在自己那一
//     页上看得见**，而且一件干完就没了。
//
// 三样都缺同一件事：**排着还没开始的看不见，干完了的也留不下**。于是页面
// 上只能报"正在画的那几张"，说不出还排着几件、刚才那件花了多久。
//
// 这本账把三个状态收在一处：
//
//   排队中 → 正在做 → 做完了 / 失败 / 取消了
//
// **一件活一个 `Task` 对象**，往栈上一放：构造登记成"排队中"，`begin()`
// 变"正在做"（开始计时），析构按结果记进"做完的"。中途抛异常也不会留下
// 幽灵行——这一条和 `Activity` 是同一个理由，那边已经验过两年。
//
// `Activity` 现在是这儿的一层壳（构造即 begin），所有老调用点一个字没改就
// 有了用时和完成记录。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pipeline/jobs.hpp"   // CancelToken

namespace changji::pipeline {

/// 一件活现在是什么状态。
enum class TaskState { Queued, Running, Done, Failed, Cancelled };

const char* to_string(TaskState s);

/// 一件活。构造即排队，`begin()` 即开工，析构即结账。
///
/// **不可拷贝、不可移动**：账本里存的是它的 id，而析构是结账的唯一出口。
/// 要放进容器就用 `std::unique_ptr<Task>`（出图那一批就是这么排的）。
class Task {
public:
    /// `kind` 是界面上那个小标签认得的类型（image / video / tts / llm /
    /// run / write），和顶栏 JobBadge 里的 KIND 表是同一套。
    ///
    /// `title` 是**这件活干什么**，要能一眼看懂：「画参考图 · 董平 正面」、
    /// 「出首帧 · 第 3 集 sh017」、「配音 · 唐海「你说过会来的」」。
    /// 用户 2026-09-17：「任务名要显示清楚干什么的」。
    ///
    /// `project` 是项目目录的绝对路径（页面靠它点过去、也靠它过滤）。
    Task(std::string kind, std::string title, std::string project = {},
         std::string episode_id = {});
    ~Task();

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task(Task&&) = delete;
    Task& operator=(Task&&) = delete;

    /// 轮到它了。**从这一刻开始计时**，页面上那个"已经用时"就是它。
    void begin();

    void set_title(std::string t);
    void set_progress(int current, int total);
    /// 盖在标题上的一层：「排队中，前面还有 2 件」。轮到了就清掉。
    void set_note(std::string n);
    /// 这件活画的是哪一格参考图。见 Activity::set_target。
    void set_target(std::string t);

    /// 大模型想到哪儿了。**覆盖式，只留最后一份**：思考流是一个字一个字
    /// 来的，每来一段存一条的话，一次写作就是几千条。页面点开看的是"到现
    /// 在为止想了什么"，那正是最后这一份。
    void set_thinking(std::string all);

    /// 又想了一段。**思考流是一段一段来的**，接起来存。
    ///
    /// 有上限（几十万字节）：一次长写作能想上万字，而这份东西还要留在
    /// "做完的"里两百条。超了就从头上截，留最近这一段——人点开看的是
    /// "它最后在想什么"。
    void append_thinking(const std::string& piece);

    /// 砸了。记一句原话，析构时按"失败"结账。
    void fail(std::string why);

    /// **这一件在长跑任务表里也有一份。**
    ///
    /// `JobTable::start` 起的那条（出片、批量写作）两边都登记：任务表那份
    /// 带着阶段和引擎现说的那句话，账本这份带着用时、思考和取消。顶栏那块
    /// 牌子把两边接成一个列表（`running_work`），**不打这个记号的话同一件
    /// 活在那儿会数两遍**——用户 2026-09-17：「后面的数字和正在做的也对
    /// 不上」，页面上「正在做 6」而顶栏写着 8，多出来的正是这两条。
    void mark_long_job();

    /// 页面上按了那个叉。
    bool cancelled() const;
    CancelToken& token();

    std::uint64_t id() const { return id_; }

private:
    std::uint64_t id_ = 0;
};

/// 按 id 改进度 / 改那句现说的话。
///
/// **给长跑任务那条用的。** 它的进度在 `JobTable` 那张表里（第几镜、第几
/// 章），而账本这份要拿来画进度条；两处各记一份必然只改一边，所以由任务表
/// 每次改完顺手同步过来。找不到（已经结完账）就什么都不做。
void set_task_progress(std::uint64_t id, int current, int total);
void set_task_note(std::uint64_t id, std::string note);

/// 页面上按了「取消」/「结束」。找不到（已经结完账了）就回 false。
///
/// **排队中的直接从队里划掉，正在做的把令牌立起来**——两种都叫"取消"，
/// 但前者当场就没了，后者要等那一层自己查令牌。
bool cancel_task(std::uint64_t id);

/// 这一刻的账：`{running:[…], queued:[…], done:[…]}`。
///
/// `project` 非空时只报那一部剧的（页面上那一页是跟着项目走的）。
/// 每一行带 `id / kind / title / note / project / episode_id / current /
/// total / seconds / thinking / error / state`。
nlohmann::json task_board(const std::string& project = {});

/// 那件活到现在为止想了什么。**单独一条路**：思考几千字，塞进上面那份
/// 每两秒推一次的账里的话，它一件就能把整条通道占满。页面点开才来取。
///
/// 还没结账的、和留在"做完的"里的都找得到；再找不到就回空串。
std::string task_thinking(std::uint64_t id);

}  // namespace changji::pipeline
