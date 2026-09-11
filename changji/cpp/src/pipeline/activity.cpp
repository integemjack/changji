#include "pipeline/activity.hpp"

#include "pipeline/jobs.hpp"

#include <map>
#include <utility>

namespace changji::pipeline {
namespace {

struct Row {
    std::string kind;
    std::string project;
    std::string episode_id;
    std::string message;
    int current = 0;
    int total = 0;
};

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
                            std::move(episode_id), std::move(message), 0, 0});
}

Activity::~Activity() {
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
            {"message", row.message},
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
