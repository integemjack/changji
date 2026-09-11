#include "pipeline/activity.hpp"

#include "pipeline/jobs.hpp"

#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace changji::pipeline {
namespace {

struct Row {
    std::string kind;
    std::string project;
    std::string episode_id;
    std::string message;
    std::string note;
    int current = 0;
    int total = 0;
};

/// 这个线程的活，最里层的在最后。
///
/// 是个栈不是一个指针：以后真出现"一件活里面套一件"的时候（比如写整季
/// 里面单独登记每一章），深处的代码该改的是最里层那件，不是最外层。
thread_local std::vector<Activity*> t_stack;

struct Registry {
    std::mutex mu;
    // **有序表，不是哈希表**：id 递增，遍历出来就是开工顺序。顶栏那个列表
    // 每两秒重画一次，行的顺序要是每次都跳，看着像有活在闪。
    std::map<std::uint64_t, Row> rows;
    std::uint64_t next = 1;
};

Registry& reg() {
    static Registry r;
    return r;
}

}  // namespace

Activity::Activity(std::string kind, std::string project, std::string episode_id,
                   std::string message) {
    Registry& r = reg();
    std::lock_guard lg(r.mu);
    id_ = r.next++;
    r.rows.emplace(id_, Row{std::move(kind), std::move(project),
                            std::move(episode_id), std::move(message), "", 0,
                            0});
    t_stack.push_back(this);
}

Activity::~Activity() {
    // **按值找，不是直接 pop_back。** 正常用法下这就是最后一个，但万一
    // 有人把 Activity 放在成员里、析构顺序不是倒着来，pop_back 会把别人
    // 的那件活从栈上抹掉，而症状是"排队中"写到了另一件活头上。
    for (auto it = t_stack.rbegin(); it != t_stack.rend(); ++it) {
        if (*it == this) {
            t_stack.erase(std::next(it).base());
            break;
        }
    }
    Registry& r = reg();
    std::lock_guard lg(r.mu);
    r.rows.erase(id_);
}

void Activity::set_message(std::string m) {
    Registry& r = reg();
    std::lock_guard lg(r.mu);
    auto it = r.rows.find(id_);
    if (it != r.rows.end()) it->second.message = std::move(m);
}

void Activity::set_progress(int current, int total) {
    Registry& r = reg();
    std::lock_guard lg(r.mu);
    auto it = r.rows.find(id_);
    if (it == r.rows.end()) return;
    it->second.current = current;
    it->second.total = total;
}

void Activity::set_note(std::string n) {
    Registry& r = reg();
    std::lock_guard lg(r.mu);
    auto it = r.rows.find(id_);
    if (it != r.rows.end()) it->second.note = std::move(n);
}

Activity* current_activity() {
    return t_stack.empty() ? nullptr : t_stack.back();
}

void note_queued(int ahead, const std::string& blocker) {
    Activity* a = current_activity();
    if (a == nullptr) return;
    if (ahead < 0) {
        a->set_note("");
        return;
    }
    if (ahead > 0) {
        a->set_note("排队中，前面还有 " + std::to_string(ahead) + " 件");
        return;
    }
    // 轮到自己了，但显存还腾不动——挡路的那个槽正被用着。
    a->set_note(blocker.empty() ? "排队中"
                                : "排队中，等「" + blocker + "」用完");
}

nlohmann::json running_activities() {
    Registry& r = reg();
    std::lock_guard lg(r.mu);
    nlohmann::json out = nlohmann::json::array();
    for (const auto& [id, row] : r.rows) {
        out.push_back({
            {"kind", row.kind},
            {"project", row.project},
            {"episode_id", row.episode_id},
            // stage 这一格短活没有，但形状要和长跑任务那边一样——
            // 前端一套代码画两边，少一个键就得在模板里到处判空。
            {"stage", ""},
            {"current", row.current},
            {"total", row.total},
            // 排队那句盖在上面。**顶栏只有一行**，两句都塞进去会挤掉
            // 后面的项目名，而正在排队的时候"在排队"比"要干什么"更要紧。
            {"message", row.note.empty() ? row.message
                                         : row.note + "：" + row.message},
            // **在跑还是在排，给个字段，别让前端去猜那句话。** 用户要的
            // 就是"正在作业的"和"排队中的"两拨分得清；靠前缀匹配中文的话，
            // 哪天那句话改一个字，界面就悄悄全算成在跑的了。
            {"queued", !row.note.empty()},
        });
    }
    return out;
}

nlohmann::json running_work() {
    // 长跑的排前面：出片、写整季这种一跑十几分钟的，才是人最想点进去看的。
    nlohmann::json out = jobs().running_jobs();
    for (auto& row : running_activities()) out.push_back(std::move(row));
    return out;
}

}  // namespace changji::pipeline
