#include "doctor/needs.hpp"

namespace changji::doctor {

ComfyNeed comfy_need(const config::Settings& s) {
    ComfyNeed n;
    n.for_video = s.models.engine == "comfy";
    n.for_tts = s.tts.backend == "comfy";
    return n;
}

bool tts_is_local(const config::Settings& s) { return s.tts.backend == "local"; }

}  // namespace changji::doctor
