#include "http/job_stream.hpp"

#include <utility>

#include "http/ws.hpp"

namespace changji::http {

void job_done(const std::string& stream_id, nlohmann::json result) {
    if (stream_id.empty()) return;
    ws::hub().broadcast(stream_id, {{"type", "job_done"},
                                    {"job_id", stream_id},
                                    {"result", std::move(result)}});
}

void job_error(const std::string& stream_id, const std::string& message) {
    if (stream_id.empty()) return;
    ws::hub().broadcast(stream_id, {{"type", "job_error"},
                                    {"job_id", stream_id},
                                    {"message", message}});
}

void job_progress(const std::string& stream_id, int current, int total,
                  const std::string& message) {
    if (stream_id.empty()) return;
    ws::hub().broadcast(stream_id, {{"type", "job_progress"},
                                    {"job_id", stream_id},
                                    {"current", current},
                                    {"total", total},
                                    {"message", message}});
}

void job_preview(const std::string& stream_id, int step,
                 std::string data_url) {
    if (stream_id.empty() || data_url.empty()) return;
    ws::hub().broadcast(stream_id, {{"type", "job_preview"},
                                    {"job_id", stream_id},
                                    {"current", step},
                                    {"image", std::move(data_url)}});
}

}  // namespace changji::http
