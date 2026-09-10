#include "pipeline/jobs.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <sstream>

using json = nlohmann::json;

namespace changji::pipeline {

namespace {

/// Unix 秒，三位小数。对齐 Python 的 round(time.time(), 3)。
double now_unix() {
    using namespace std::chrono;
    const auto d = system_clock::now().time_since_epoch();
    const double s = duration<double>(d).count();
    return std::nearbyint(s * 1000.0) / 1000.0;
}

/// 一位小数。对齐 Python 的 round(elapsed, 1)。
double round1(double x) { return std::nearbyint(x * 10.0) / 10.0; }

std::string new_job_id(JobKind kind) {
    static std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream os;
    os << to_string(kind) << "-" << std::hex << rng();
    return os.str();
}

json opt_str(const std::optional<std::string>& v) {
    return v.has_value() ? json(*v) : json(nullptr);
}

}  // namespace

const char* stopped_message(JobKind k) {
    return k == JobKind::Run ? kRunStoppedMessage : kWriteStoppedMessage;
}

const char* to_string(JobKind k) {
    switch (k) {
        case JobKind::Run:   return "run";
        case JobKind::Write: return "write";
    }
    return "?";
}

json Event::to_json() const {
    return {
        {"at", at},
        {"stage", stage},
        {"kind", kind},
        {"message", message},
        {"shot_id", opt_str(shot_id)},
        {"current", current},
        {"total", total},
    };
}

JobTable::JobTable() = default;

JobTable::~JobTable() {
    // 先请求取消再等。不取消的话析构会卡在一个可能跑几十分钟的任务上。
    run_.token.request();
    write_.token.request();
    wait_idle();
    if (run_.worker.joinable()) run_.worker.join();
    if (write_.worker.joinable()) write_.worker.join();
}

JobTable::Slot& JobTable::slot(JobKind k) {
    return k == JobKind::Run ? run_ : write_;
}
const JobTable::Slot& JobTable::slot(JobKind k) const {
    return k == JobKind::Run ? run_ : write_;
}

bool JobTable::start(JobKind kind, const std::string& episode_id, Body body,
                     const std::string& stop_message) {
    std::unique_lock lk(mu_);
    Slot& s = slot(kind);
    if (s.state.running) return false;

    // 先占坑再干别的。下面 join 上一轮线程时要临时解锁，
    // 不先把 running 立起来的话，那个窗口里第二个 start() 会看到
    // running==false 一起挤进来，两条线程抢同一个槽。
    s.state.running = true;

    // 上一轮的线程可能还没退——正常跑完没来得及 join，或者被手动停止后
    // 还在收尾（那种情况 running 早就是 false 了）。这里一定要等到它真的结束：
    // 不等的话 std::thread 的赋值运算符会调 terminate，
    // 而且新旧两条线程会同时写同一个 state。
    if (s.worker.joinable()) {
        lk.unlock();
        s.worker.join();
        lk.lock();
    }

    s.token.reset();
    s.state = JobState{};
    s.state.running = true;
    s.state.job_id = new_job_id(kind);
    if (!episode_id.empty()) s.state.episode_id = episode_id;
    s.state.started_at = std::chrono::steady_clock::now();
    s.state.stop_message = stop_message;
    s.active = true;

    const std::string job_id = s.state.job_id;

    s.worker = std::thread([this, kind, job_id, body = std::move(body)]() {
        JobProgress progress(this, kind);
        try {
            body(progress);
        } catch (const std::exception& e) {
            std::lock_guard lg(mu_);
            slot(kind).state.error = e.what();
        } catch (...) {
            std::lock_guard lg(mu_);
            slot(kind).state.error = "未知异常";
        }

        json final_msg;
        {
            std::lock_guard lg(mu_);
            Slot& sl = slot(kind);
            sl.state.running = false;
            sl.active = false;
            // 跑完了就没有"还没落定"的了。留着的话下一次页面一进来，
            // 会把上一轮剩下的当成还在排队。
            sl.state.pending.clear();
            // done / error 是终止消息，**不受节流影响**——
            // 被节流掉的话前端会永远停在"跑着"的状态。
            //
            // 注意判断顺序：error 先于 cancelled。手动停止时 cancel() 已经
            // 把 error 写成了那句"已完成的镜头会保留"，这里不能再覆盖成"已取消"。
            if (sl.state.error.has_value()) {
                final_msg = {{"type", "error"}, {"job_id", job_id},
                             {"message", *sl.state.error}};
            } else if (sl.token.cancelled()) {
                final_msg = {{"type", "error"}, {"job_id", job_id},
                             {"message", "已取消"}};
            } else {
                final_msg = {{"type", "done"}, {"job_id", job_id},
                             {"outputs", sl.state.outputs}};
            }
        }
        emit(job_id, final_msg);
        idle_cv_.notify_all();
    });

    return true;
}

void JobTable::set_sink(Sink s) {
    std::lock_guard lg(mu_);
    sink_ = std::move(s);
}

void JobTable::emit(const std::string& job_id, const json& msg) const {
    // 取一份拷贝再调用，别拿着锁进 sink——sink 里是 Hub，Hub 自己有锁。
    Sink s;
    {
        std::lock_guard lg(mu_);
        s = sink_;
    }
    if (s) s(job_id, msg);
}

bool JobTable::cancel(JobKind kind) {
    std::lock_guard lg(mu_);
    Slot& s = slot(kind);
    if (!s.state.running) return false;
    s.token.request();

    // running 立刻置 false，不等工作线程真的退出。
    //
    // 这是抄 Python 的：那边 stop_run() 里是 task.cancel() 之后紧跟着
    // state.running = False。差别是可观测的——前端点完停止马上会拉一次
    // /api/run，如果这里还报 running=true，界面就会卡在"正在跑"上好几秒
    // （工作线程要跑到下一个取消检查点才退）。
    //
    // 代价是 running 不再等价于"线程还活着"。所以 start() 里那句 join
    // 不能删：槽看着空了，上一条线程可能还在收尾。
    s.state.running = false;
    s.state.pending.clear();
    // 任务自己指定的那句优先。同一个槽上跑的两件事说法不一样：
    // 写整季停了是"已经写好的几集留着"，批量出分镜停了是"已经出好的分镜留着"。
    s.state.error = s.state.stop_message.empty() ? stopped_message(kind)
                                                 : s.state.stop_message;
    return true;
}

void JobTable::record(JobKind kind, Event ev) {
    std::string job_id;
    json msg;

    // 预览图**只广播**：不进事件环（几十 KB 一张，环放不下），不动进度
    // （它不是一步，是一步中间的样子），不进 /api/run（和 Python 对拍）。
    // 老客户端不认识 kind = preview，按 progress 处理也只是多刷一次状态。
    if (ev.kind == "preview") {
        {
            std::lock_guard lg(mu_);
            job_id = slot(kind).state.job_id;
        }
        emit(job_id, json{{"type", "progress"},
                          {"kind", "preview"},
                          {"job_id", job_id},
                          {"stage", ev.stage},
                          {"shot_id", ev.shot_id.value_or("")},
                          {"step", ev.current},
                          {"preview", ev.preview}});
        return;
    }
    {
        std::lock_guard lg(mu_);
        Slot& s = slot(kind);
        JobState& st = s.state;

        // 事件时间戳由这里统一打，调用方不用管。Python 那边是 append 时
        // 取 round(time.time(),3)，同一个位置。
        if (ev.at == 0.0) ev.at = now_unix();

        // 只有带总数的事件才更新进度。不加这个判断的话，
        // 一条 total=0 的日志事件会把进度条清零。
        if (ev.total) {
            // **同一阶段里 current 只进不退。** 多卡时几镜同时在跑，
            // 各自的进度事件带的是自己的序号：3、11、7、12……原样写进去
            // 进度条就来回蹦。Python 那边是串行的，序号天然单调，
            // 所以它直接赋值也对；这里加一道 max，串行时结果一个字不差。
            // 换了阶段就从头来——新阶段的 1/12 当然要比上一阶段的 12/12 小。
            const bool same_stage = st.stage == ev.stage;
            st.current = same_stage ? std::max(st.current, ev.current) : ev.current;
            st.total = ev.total;
        }
        st.stage = ev.stage;
        st.message = ev.message;

        st.events.push_back(ev);
        while (st.events.size() > kMaxEvents) st.events.pop_front();

        // 一镜落定（shot_done / warn / gate……任何带 shot_id 的非 progress）
        // 就从"还没落定"里划掉。和界面 trackInflight 的判据一字不差——
        // 两边判据不一样的话，刷新前后同一镜的「排队中」会不一样。
        if (ev.shot_id.has_value() && ev.kind != "progress") {
            auto& pend = st.pending;
            pend.erase(std::remove(pend.begin(), pend.end(), *ev.shot_id),
                       pend.end());
        }

        job_id = st.job_id;
        msg = {
            {"type", ev.kind == "error" ? "error" : "progress"},
            // 原样带上 kind。type 只分 progress / error 两种，warn、gate、
            // shot_done 到了界面全成了 "progress"——界面因此分不出
            // 一个镜头是"还在跑"还是"跑完了"。多卡之后六镜同时在跑，
            // 不带这个字段界面上就是六条进度轮流刷同一个位置。
            // **加字段不改旧字段**，老客户端照旧。
            {"kind", ev.kind},
            {"job_id", job_id},
            {"stage", ev.stage},
            {"step", ev.current},
            {"total", ev.total},
            {"message", ev.message},
        };
        if (ev.shot_id.has_value()) msg["shot_id"] = *ev.shot_id;
        // 这一镜自己的进度。**只在这条路上给**，`/api/run` 的事件数组
        // 要和 Python 一字不差。见 Event::shot_steps 的注释。
        // 没有就不加：镜头墙靠"有没有这两个字段"决定画不画那条进度条，
        // 补个 0 会让每张牌上都挂一条永远空着的槽。
        if (ev.shot_steps > 0) {
            msg["shot_step"] = ev.shot_step;
            msg["shot_steps"] = ev.shot_steps;
            msg["shot_prep"] = ev.shot_prep;
        } else if (ev.shot_prep) {
            // **还没进采样：没有步数，但"正在准备"这件事要说。**
            // 借槽那一下是阻塞的——可能先卸大模型腾地方，再从磁盘读
            // 十几二十 GB 进来，几十秒起。这一整段 sd.cpp 还没跑，
            // 它的进度回调一次都不触发，牌子上就是一动不动。
            // 只给这个标志、不给步数：上面那条"补个 0 会挂一条空进度槽"
            // 的规矩还在，进度条照样不画。
            msg["shot_prep"] = true;
        }
    }
    // 广播放在锁外：Hub 自己有锁，嵌套两把锁是死锁的常见来源。
    emit(job_id, msg);
}

std::vector<std::string> JobTable::pending(JobKind kind) const {
    std::lock_guard lg(mu_);
    return slot(kind).state.pending;
}

json JobTable::snapshot(JobKind kind) const {
    std::lock_guard lg(mu_);
    const JobState& s = slot(kind).state;

    json events = json::array();
    const std::size_t skip =
        s.events.size() > kSnapshotEvents ? s.events.size() - kSnapshotEvents : 0;
    for (std::size_t i = skip; i < s.events.size(); ++i) {
        events.push_back(s.events[i].to_json());
    }

    const double elapsed =
        s.started_at.time_since_epoch().count() == 0
            ? 0.0
            : round1(std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - s.started_at).count());

    if (kind == JobKind::Write) {
        // WriteState.snapshot() 的字段少得多，别把 Run 的那些混进来
        return {
            {"running", s.running},
            {"done", s.done},
            {"total", s.total},
            {"message", s.message},
            {"episodes", s.episodes},
            {"error", opt_str(s.error)},
        };
    }

    return {
        {"running", s.running},
        {"episode_id", opt_str(s.episode_id)},
        {"stage", s.stage},
        {"current", s.current},
        {"total", s.total},
        {"message", s.message},
        {"elapsed_s", elapsed},
        {"output", opt_str(s.output)},
        {"outputs", s.outputs},
        {"queue_done", s.queue_done},
        {"queue_total", s.queue_total},
        {"error", opt_str(s.error)},
        {"events", events},
    };
}

bool JobTable::running(JobKind kind) const {
    std::lock_guard lg(mu_);
    return slot(kind).state.running;
}

std::string JobTable::job_id(JobKind kind) const {
    std::lock_guard lg(mu_);
    return slot(kind).state.job_id;
}

void JobTable::wait_idle() {
    std::unique_lock lk(mu_);
    idle_cv_.wait(lk, [this] { return !run_.active && !write_.active; });
}


// ---- JobProgress ----

template <typename F>
void JobTable::mutate(JobKind kind, F&& fn) {
    std::lock_guard lg(mu_);
    fn(slot(kind).state);
}

void JobProgress::report(Event ev) { table_->record(kind_, std::move(ev)); }

void JobProgress::set_message(std::string m) {
    table_->mutate(kind_, [&](JobState& s) { s.message = std::move(m); });
}

void JobProgress::set_done(int done) {
    table_->mutate(kind_, [&](JobState& s) { s.done = done; });
}

void JobProgress::set_total(int total) {
    table_->mutate(kind_, [&](JobState& s) { s.total = total; });
}

void JobProgress::add_episode(nlohmann::json ep) {
    table_->mutate(kind_,
                   [&](JobState& s) { s.episodes.push_back(std::move(ep)); });
}

void JobProgress::set_pending(std::vector<std::string> shot_ids) {
    table_->mutate(kind_, [&](JobState& s) { s.pending = std::move(shot_ids); });
}

void JobProgress::set_output(std::string path) {
    table_->mutate(kind_, [&](JobState& s) { s.output = std::move(path); });
}

void JobProgress::add_output(std::string path) {
    table_->mutate(kind_,
                   [&](JobState& s) { s.outputs.push_back(std::move(path)); });
}

void JobProgress::set_episode_id(std::string id) {
    table_->mutate(kind_, [&](JobState& s) { s.episode_id = std::move(id); });
}

void JobProgress::set_queue(int done, int total) {
    table_->mutate(kind_, [&](JobState& s) {
        s.queue_done = done;
        s.queue_total = total;
    });
}

void JobProgress::set_error(std::string e) {
    table_->mutate(kind_, [&](JobState& s) { s.error = std::move(e); });
}

bool JobProgress::cancelled() const {
    std::lock_guard lg(table_->mu_);
    return table_->slot(kind_).token.cancelled();
}

CancelToken& JobProgress::token() { return table_->slot(kind_).token; }

JobTable& jobs() {
    static JobTable table;
    return table;
}

}  // namespace changji::pipeline
