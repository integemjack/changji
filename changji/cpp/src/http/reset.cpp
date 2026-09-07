#include "http/reset.hpp"

namespace changji::http {

using namespace changji::models;

int reset_all_shots(const ProjectStore& store) {
    Project project = store.load_project();
    int n = 0;
    for (auto& ep : project.episodes) {
        for (auto& shot : ep.shots) {
            if (shot.status == ShotStatus::PLANNED ||
                shot.status == ShotStatus::LOCKED) {
                continue;
            }
            shot.status = ShotStatus::PLANNED;
            shot.attempts = 0;
            shot.gate_notes.clear();
            ++n;
        }
    }
    store.save_project(project);
    return n;
}

}  // namespace changji::http
