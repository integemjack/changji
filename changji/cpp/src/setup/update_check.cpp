#include "setup/update_check.hpp"

#include "util/text.hpp"

namespace changji::setup {

std::string version_json_url(const config::UpdateConfig& cfg) {
    const std::string repo =
        cfg.repo.empty() ? std::string("integemjack/changji") : cfg.repo;
    const std::string ch =
        cfg.channel == "beta" ? std::string("beta") : std::string("release");
    return "https://github.com/" + repo + "/releases/download/" + ch +
           "/version.json";
}

bool is_different_version(const std::string& current,
                          const std::string& latest) {
    const std::string a = text::strip_ws(current);
    const std::string b = text::strip_ws(latest);
    // 任一边不知道就当没有新版：**报一个假的"有新版"比不报更糟**，
    // 人点过去发现下不到。
    if (a.empty() || b.empty()) return false;
    return a != b;
}

UpdateInfo check_update(const config::UpdateConfig& cfg,
                        const std::string& current, const Fetch& fetch) {
    UpdateInfo out;
    out.current = text::strip_ws(current);
    out.url = "https://github.com/" +
              (cfg.repo.empty() ? std::string("integemjack/changji") : cfg.repo) +
              "/releases/" + (cfg.channel == "beta" ? "tag/beta" : "tag/release");
    if (!fetch) {
        out.error = "没法发请求";
        return out;
    }
    const std::string body = fetch(version_json_url(cfg));
    if (body.empty()) {
        out.error = "取不到版本信息（" + version_json_url(cfg) + "）";
        return out;
    }
    const auto js = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (js.is_discarded() || !js.is_object()) {
        // **不把原文贴出来。** 取到的多半是一张 404 页面，几十 KB 的 HTML
        // 摆进界面没人读得下去。
        out.error = "那头回的不是版本信息";
        return out;
    }
    out.latest = text::strip_ws(js.value("version", std::string()));
    out.built_at = js.value("built_at", std::string());
    if (out.latest.empty()) {
        out.error = "版本信息里没有 version 这一栏";
        return out;
    }
    out.newer = is_different_version(out.current, out.latest);
    return out;
}

}  // namespace changji::setup
