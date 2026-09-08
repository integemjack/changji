#include "http/voices.hpp"

#include <array>
#include <vector>

#include "comfy/loader.hpp"
#include "models/project.hpp"
#include "util/paths.hpp"

using json = nlohmann::json;

namespace changji::http {

namespace {

/// 音色那个输入可能叫什么。顺序就是优先级。
///
/// 逐字抄 Python 的 _VOICE_KEYS。这几个名字来自实际用过的几个 TTS 节点包，
/// 不是猜的——加一个名字之前先确认真有节点用它，不然只会误伤：
/// 一个叫 "speaker" 的字符串输入被当成下拉框，列出来的是空表。
constexpr std::array<const char*, 6> kVoiceKeys = {
    "narrator_voice", "voice", "voice_id", "speaker", "reference_audio",
    "speaker_id",
};

}  // namespace

std::optional<std::pair<std::string, std::string>> find_voice_input(
    const comfy::ApiWorkflow& w) {
    const auto& prompt = w.to_json();
    if (!prompt.is_object()) return std::nullopt;

    // 按工作流里的节点顺序找。**外层是节点、内层是键名**，
    // 反过来的话优先级就成了"哪个键名靠前"而不是"哪个节点靠前"，
    // 一个工作流里有两个 TTS 节点时挑中的是另一个。
    for (const auto& kv : prompt.items()) {
        const auto cls = kv.value().find("class_type");
        if (cls == kv.value().end() || !cls->is_string()) continue;
        const auto inputs = kv.value().find("inputs");
        if (inputs == kv.value().end() || !inputs->is_object()) continue;
        for (const char* key : kVoiceKeys) {
            if (inputs->contains(key)) {
                return std::make_pair(cls->get<std::string>(), std::string(key));
            }
        }
    }
    return std::nullopt;
}

ApiResult get_voices(const std::string& path, comfy::Client& client,
                     const std::string& backend) {
    if (path.empty()) throw ApiError(400, "没有指定项目目录");

    // 进程内配音：没有服务端清单可列，音色是一段参考音频。
    // 回空列表 + 一句说明，而不是去问一个根本没在用的后端。
    if (backend == "local") {
        return {200,
                {{"voices", json::array()},
                 {"error", "进程内配音（[tts].backend = local）没有音色清单。"
                           "音色来自参考音频：把角色的 voice_id 填成一段人声"
                           "片段的路径，模型照着它的音色念。"}}};
    }

    const models::ProjectStore store(paths::from_utf8(path));

    std::optional<comfy::ApiWorkflow> wf;
    try {
        // 项目里放了自己的 tts.json 就用项目的，音色列表跟着它走。
        wf = comfy::load_workflow(client, store, "tts");
    } catch (const std::exception& e) {
        return {200, {{"voices", json::array()},
                      {"error", std::string("读不到配音工作流：") + e.what()}}};
    }
    if (!wf.has_value()) {
        return {200, {{"voices", json::array()},
                      {"error", "读不到配音工作流：项目和内置里都没有 tts.json"}}};
    }

    const auto where = find_voice_input(*wf);
    if (!where.has_value()) {
        return {200, {{"voices", json::array()},
                      {"error", "配音工作流里没找到音色输入。"
                                "确认 workflows/tts.json 里的节点有 voice "
                                "或者 speaker 之类的输入"}}};
    }

    std::vector<std::string> names;
    try {
        names = client.available_models(where->first, where->second);
    } catch (const std::exception& e) {
        // **这里和 Python 不一样，是有意的。** 那边 list_voices 把异常吞掉
        // 返回空列表，于是"服务端连不上"和"一个音色都没装"回的是同一个
        // 空列表——用户看到的是一个空下拉框，没有任何线索。
        // 前端本来就会显示 error 字段（角色页那段代码已经在读它了）。
        return {200, {{"voices", json::array()},
                      {"error", std::string("问不到服务端：") + e.what()}}};
    }

    json out = json::array();
    for (const auto& n : names) {
        // "none" 是节点用来表示"不指定"的占位值，不是一个音色。
        // 留着的话下拉框里会多出一项，选了等于没选。
        if (n.empty() || n == "none") continue;
        out.push_back(n);
    }
    return {200, {{"voices", out}}};
}

}  // namespace changji::http
