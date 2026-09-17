#include "pipeline/task_board.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <cmath>
#include <mutex>
#include <utility>
#include <vector>

namespace changji::pipeline {
namespace {

using Clock = std::chrono::steady_clock;

/// 做完的留多少条。
///
/// **一条几百字节，两百条就是几十 KB**，而这份东西会整份推给浏览器。
/// 再多的价值也不大：人回头看的是"刚才那几件花了多久"，不是上周的流水。
constexpr std::size_t kDoneKeep = 200;

/// 一件活的思考最多留多少字节。**两百条 × 这个数**是这本账的上限。
/// 20 万字节大约六七万汉字，比任何一次写作想的都多。
constexpr std::size_t kThinkingKeep = 200000;

struct Row {
    std::uint64_t id = 0;
    std::string kind;
    std::string title;
    std::string note;
    std::string project;
    std::string episode_id;
    std::string target;
    std::string thinking;
    std::string error;
    TaskState state = TaskState::Queued;
    /// 长跑任务表里也有一份，见 Task::mark_long_job。
    bool long_job = false;
    int current = 0;
    int total = 0;
    Clock::time_point queued_at{};
    Clock::time_point started_at{};
    Clock::time_point ended_at{};
    CancelToken tok;
};

struct Board {
    std::mutex mu;
    // **有序表**：id 递增，遍历出来就是登记顺序。页面每两秒重画一次，
    // 行的顺序要是每次都跳，看着像有活在闪。
    std::map<std::uint64_t, std::shared_ptr<Row>> live;
    std::deque<std::shared_ptr<Row>> done;
    std::uint64_t next = 1;

    /// 每一种活最近几次真花了多久。排队那几件的"预计什么时候开始"靠它。
    std::map<std::string, std::deque<double>> recent;
};

Board& board() {
    static Board b;
    return b;
}

double secs(Clock::time_point a, Clock::time_point b) {
    if (a.time_since_epoch().count() == 0) return 0.0;
    return std::chrono::duration<double>(b - a).count();
}

double round1(double v) { return std::round(v * 10.0) / 10.0; }

/// 同一族活按什么归类算耗时。
///
/// **不能只按 kind。** `image` 这一族里既有「画参考图 · 董平 正面」也有
/// 「出首帧 · ep08_sh001」，两者的耗时不是一个量级（参考图一分钟上下，
/// 首帧带着参考图去编辑要更久）。混在一个桶里算出来的中位数，报给谁都不对。
///
/// 标题里 `· ` 前面那一截正是"这是哪一族"（「画参考图」「出首帧」
/// 「出片成片档」「配音」），拿它当桶名。没有那个分隔就退回 kind。
std::string bucket_of(const Row& r) {
    const auto pos = r.title.find(" · ");
    return pos == std::string::npos ? r.kind : r.kind + "/" + r.title.substr(0, pos);
}

/// 这一族活一件大概多久。没跑过就回 0（页面上不报预计）。
/// **取中位数不是平均**：一件卡住的（等显存、等对面机器）能把平均拖成两倍。
double typical_locked(Board& b, const std::string& kind) {
    auto it = b.recent.find(kind);
    if (it == b.recent.end() || it->second.empty()) return 0.0;
    std::vector<double> v(it->second.begin(), it->second.end());
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

nlohmann::json to_json_locked(Board& b, const Row& r, Clock::time_point now) {
    const double elapsed =
        r.state == TaskState::Running
            ? secs(r.started_at, now)
            : secs(r.started_at, r.ended_at);
    nlohmann::json j = {
        {"id", r.id},
        {"kind", r.kind},
        {"title", r.title},
        {"note", r.note},
        {"project", r.project},
        {"episode_id", r.episode_id},
        {"target", r.target},
        {"state", to_string(r.state)},
        {"current", r.current},
        {"total", r.total},
        {"seconds", round1(elapsed)},
        {"error", r.error},
        // **思考只报有没有，不报正文**：一次写作的思考几千字，而这份账两秒
        // 推一次。页面点开那一下再单独去取（见 /api/task/thinking）。
        {"thinking", !r.thinking.empty()},
        {"thinking_chars", static_cast<int>(r.thinking.size())},
        // **按了叉之后它还在名单上待一会儿。**
        //
        // 排着的那几件是"有空位了才被领走"的，取消只是把令牌立起来——真正
        // 划掉要等领它的那一路回头看一眼。中间这段（最长就是一件活的时间）
        // 名单上还有它，不说一声的话看着像那个叉没按上。
        {"cancelling", r.tok.cancelled() &&
                           (r.state == TaskState::Queued ||
                            r.state == TaskState::Running)},
    };
    if (r.state == TaskState::Queued) {
        // 排了多久了。**光有"预计还要等多久"不够**：等得久的那几件，人想
        // 知道的是"它是不是被忘了"。
        j["waited"] = round1(secs(r.queued_at, now));
        j["eta"] = round1(typical_locked(b, bucket_of(r)));
    }
    return j;
}

}  // namespace

const char* to_string(TaskState s) {
    switch (s) {
        case TaskState::Queued: return "queued";
        case TaskState::Running: return "running";
        case TaskState::Done: return "done";
        case TaskState::Failed: return "failed";
        case TaskState::Cancelled: return "cancelled";
    }
    return "queued";
}

Task::Task(std::string kind, std::string title, std::string project,
           std::string episode_id) {
    Board& b = board();
    std::lock_guard lg(b.mu);
    id_ = b.next++;
    auto row = std::make_shared<Row>();
    row->id = id_;
    row->kind = std::move(kind);
    row->title = std::move(title);
    row->project = std::move(project);
    row->episode_id = std::move(episode_id);
    row->queued_at = Clock::now();
    b.live.emplace(id_, std::move(row));
}

Task::~Task() {
    Board& b = board();
    std::lock_guard lg(b.mu);
    auto it = b.live.find(id_);
    if (it == b.live.end()) return;
    auto row = it->second;
    b.live.erase(it);
    row->ended_at = Clock::now();
    if (row->state == TaskState::Running) {
        // **令牌立着就是被取消的，不是失败的。** 「人按的停不是失败」这条
        // 规矩在引擎里到处都写着（ref_gen.cpp、util/cancel_words.hpp），
        // 账本这头也得分清——不然页面上一排红的，而什么都没出错。
        row->state = row->tok.cancelled() ? TaskState::Cancelled
                     : row->error.empty() ? TaskState::Done
                                          : TaskState::Failed;
        if (row->state == TaskState::Done) {
            auto& q = b.recent[bucket_of(*row)];
            q.push_back(secs(row->started_at, row->ended_at));
            if (q.size() > 20) q.pop_front();
        }
    } else {
        // 没 begin 过就没了 = 排着的时候被取消了。
        row->state = TaskState::Cancelled;
    }
    b.done.push_back(std::move(row));
    while (b.done.size() > kDoneKeep) b.done.pop_front();
}

void Task::begin() {
    Board& b = board();
    std::lock_guard lg(b.mu);
    auto it = b.live.find(id_);
    if (it == b.live.end()) return;
    it->second->state = TaskState::Running;
    it->second->started_at = Clock::now();
    it->second->note.clear();
}

#define CHANGJI_TASK_MUTATE(body)                     \
    Board& b = board();                               \
    std::lock_guard lg(b.mu);                         \
    auto it = b.live.find(id_);                       \
    if (it == b.live.end()) return;                   \
    Row& row = *it->second;                           \
    body

void Task::set_title(std::string t) { CHANGJI_TASK_MUTATE(row.title = std::move(t);) }
void Task::set_note(std::string n) { CHANGJI_TASK_MUTATE(row.note = std::move(n);) }
void Task::set_target(std::string t) { CHANGJI_TASK_MUTATE(row.target = std::move(t);) }
void Task::set_thinking(std::string a) { CHANGJI_TASK_MUTATE(row.thinking = std::move(a);) }
void Task::append_thinking(const std::string& piece) {
    CHANGJI_TASK_MUTATE(
        row.thinking += piece;
        if (row.thinking.size() > kThinkingKeep) {
            row.thinking.erase(0, row.thinking.size() - kThinkingKeep);
        })
}
void Task::fail(std::string why) { CHANGJI_TASK_MUTATE(row.error = std::move(why);) }
void Task::mark_long_job() { CHANGJI_TASK_MUTATE(row.long_job = true;) }
void Task::set_progress(int current, int total) {
    CHANGJI_TASK_MUTATE(row.current = current; row.total = total;)
}

#undef CHANGJI_TASK_MUTATE

bool Task::cancelled() const {
    Board& b = board();
    std::lock_guard lg(b.mu);
    auto it = b.live.find(id_);
    return it != b.live.end() && it->second->tok.cancelled();
}

CancelToken& Task::token() {
    Board& b = board();
    std::lock_guard lg(b.mu);
    auto it = b.live.find(id_);
    // 已经结完账的：给一个哑元，调用方照样查得动。和 job_stream.hpp 里
    // `current_cancel()` 没有 JobScope 时那条是同一个做法。
    static CancelToken dummy;
    return it == b.live.end() ? dummy : it->second->tok;
}

std::string task_thinking(std::uint64_t id) {
    Board& b = board();
    std::lock_guard lg(b.mu);
    auto it = b.live.find(id);
    if (it != b.live.end()) return it->second->thinking;
    for (const auto& row : b.done) {
        if (row->id == id) return row->thinking;
    }
    return {};
}

bool cancel_task(std::uint64_t id) {
    Board& b = board();
    std::lock_guard lg(b.mu);
    auto it = b.live.find(id);
    if (it == b.live.end()) return false;
    it->second->tok.request();
    return true;
}

nlohmann::json running_activities() {
    // 顶栏那一行要的形状和长跑任务那边一样（见 JobTable::running_jobs）。
    // **只报短活**：长跑任务由 jobs() 那头报，两边在 running_work() 里接起来。
    Board& b = board();
    std::lock_guard lg(b.mu);
    nlohmann::json out = nlohmann::json::array();
    for (const auto& [id, row] : b.live) {
        if (row->state != TaskState::Running) continue;
        // **长跑那几条跳过**：`running_work` 把这份和任务表那份接成一个
        // 列表，不跳的话同一件活在顶栏上数两遍。见 Task::mark_long_job。
        if (row->long_job) continue;
        out.push_back({
            {"kind", row->kind},
            {"project", row->project},
            {"episode_id", row->episode_id},
            // stage 这一格短活没有，但形状要和长跑任务那边一样——
            // 前端一套代码画两边，少一个键就得在模板里到处判空。
            {"stage", ""},
            // 画的是哪一格参考图。**没有 WebSocket 的时候设定页就靠它**
            // 认出那一格在画（见前端 useRefStream）。别的活是空串。
            {"target", row->target},
            {"current", row->current},
            {"total", row->total},
            // 排队那句盖在上面。**顶栏只有一行**，两句都塞进去会挤掉
            // 后面的项目名，而正在排队的时候"在排队"比"要干什么"更要紧。
            {"message", row->note.empty() ? row->title
                                          : row->note + "：" + row->title},
            // **在跑还是在排，给个字段，别让前端去猜那句话。**
            {"queued", !row->note.empty()},
        });
    }
    return out;
}

nlohmann::json task_board(const std::string& project) {
    Board& b = board();
    std::lock_guard lg(b.mu);
    const auto now = Clock::now();
    nlohmann::json running = nlohmann::json::array();
    nlohmann::json queued = nlohmann::json::array();
    nlohmann::json done = nlohmann::json::array();
    const auto mine = [&project](const Row& r) {
        return project.empty() || r.project.empty() || r.project == project;
    };
    for (const auto& [id, row] : b.live) {
        if (!mine(*row)) continue;
        (row->state == TaskState::Running ? running : queued)
            .push_back(to_json_locked(b, *row, now));
    }
    // 做完的**倒着报**：刚干完那件在最上面。
    for (auto it = b.done.rbegin(); it != b.done.rend(); ++it) {
        if (!mine(**it)) continue;
        done.push_back(to_json_locked(b, **it, now));
    }
    return {{"running", running}, {"queued", queued}, {"done", done}};
}

}  // namespace changji::pipeline
