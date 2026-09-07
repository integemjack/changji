#include "stages/storyboard.hpp"

#include "stages/limits.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <string>
#include <vector>

#include "stages/json_extract.hpp"
#include "stages/shot_schema.inc.hpp"
#include "stages/storyboard_prompt.inc.hpp"
#include "util/text.hpp"

using json = nlohmann::json;
using ordered = nlohmann::ordered_json;

namespace changji::stages {

using namespace changji::models;

namespace {

/// 对应 Python 的 f"{x:g}"。
///
/// C 的 %g 和 Python 的 :g 是同一套规则（默认 6 位有效数字，
/// 指数超出 [-4, 6) 才转科学计数法），所以直接用。
/// 提示词里的"总时长 300 秒"就靠它——写成 300.0 秒就破契约了。
std::string format_g(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return std::string(buf);
}

/// Python 的 round()：**银行家舍入**，不是四舍五入。
///
/// round(0.5) 是 0 不是 1，round(2.5) 是 2 不是 3。配额分配里用了三次，
/// 用错的话镜头数会差一个——差一个就意味着提示词里的配额和实际不符，
/// 模型会照着提示词产出，然后校验不过。
///
/// std::nearbyint 在默认舍入模式（FE_TONEAREST）下就是这个语义。
long py_round(double v) { return static_cast<long>(std::nearbyint(v)); }

/// 从 pydantic 导出的 Shot schema，解析一次缓存起来。
const ordered& shot_schema_base() {
    static const ordered base =
        ordered::parse(prompt::kShotSchemaJson);
    return base;
}

/// 分镜阶段允许大模型填的字段。
const std::set<std::string>& llm_shot_fields() {
    static const std::set<std::string> kFields = [] {
        std::set<std::string> s;
        for (const char* f : prompt::kLlmShotFields) s.insert(f);
        return s;
    }();
    return kFields;
}

/// 取 JSON 里的字符串，缺了或不是字符串就给默认值。
std::string str_or(const json& obj, const char* key, const std::string& def = {}) {
    if (!obj.is_object()) return def;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return def;
    return it->get<std::string>();
}

/// 对应 Python 的真值判断：缺失、null、0、空串、空数组都算假。
bool truthy(const json& obj, const char* key) {
    if (!obj.is_object()) return false;
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return false;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number()) return it->get<double>() != 0.0;
    if (it->is_string()) return !it->get<std::string>().empty();
    if (it->is_array() || it->is_object()) return !it->empty();
    return true;
}

std::string join_lines(const std::vector<std::string>& parts) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += "\n";
        out += parts[i];
    }
    return out;
}

}  // namespace

double max_shot_duration_s(int fps) {
    return static_cast<double>(kMaxFrames) / static_cast<double>(fps);
}

const std::vector<double>& duration_slots() {
    static const std::vector<double> kSlots = [] {
        // Python 那边是模块级常量，用**默认 fps=24** 算的，
        // 配置里改了 assembly.fps 也不会重算。照抄这个行为，
        // 不是因为它对，而是因为改了两边的分镜表就对不上了。
        const double limit = max_shot_duration_s();
        const double all[] = {2.0, 3.0, 4.0, 5.0, 8.0, 10.0};
        std::vector<double> out;
        for (const double s : all) {
            if (s <= limit) out.push_back(s);
        }
        if (out.empty()) out.push_back(2.0);  // 对应 Python 的 or (2.0,)
        return out;
    }();
    return kSlots;
}

double DurationQuota::total_s() const {
    // 求和顺序要和 Python 一致。那边 slots 是插入序，而 for_duration
    // 的插入序正好是档位升序，和 std::map 的遍历序相同。
    double sum = 0.0;
    for (const auto& [d, n] : slots) sum += d * n;
    return sum;
}

int DurationQuota::shot_count() const {
    int sum = 0;
    for (const auto& [d, n] : slots) {
        (void)d;
        sum += n;
    }
    return sum;
}

std::string DurationQuota::describe() const {
    std::vector<std::string> parts;
    for (const auto& [d, n] : slots) {          // map 已经是升序
        if (!n) continue;                        // 对应 Python 的 if n
        parts.push_back(std::to_string(n) + " 个 " + format_g(d) + " 秒镜头");
    }
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += "，";
        out += parts[i];
    }
    return out;
}

DurationQuota DurationQuota::for_duration(double target_s) {
    if (target_s <= 0) throw StoryboardError("目标时长必须大于 0");

    // 分配比例。只用真正可生成的档位，长镜头多分一点时长，
    // 短镜头多分一点数量，这样节奏有变化而不是平铺。
    const std::vector<std::pair<double, double>> weights = {
        {2.0, 0.10}, {3.0, 0.22}, {4.0, 0.18}, {5.0, 0.50}};
    const std::vector<double>& all = duration_slots();

    std::vector<std::pair<double, double>> plan;
    for (const auto& [d, w] : weights) {
        if (std::find(all.begin(), all.end(), d) != all.end()) {
            plan.emplace_back(d, w);
        }
    }
    if (plan.empty()) plan.emplace_back(all.back(), 1.0);

    // 按插入序累加。0.10+0.22+0.18+0.50 在浮点下**不等于** 1.0
    // （是 1.0000000000000002），换个顺序加结果又不一样。
    // scale 会因此差一个 ulp，而后面要过 round()——正好卡在半整数上时
    // 那一个 ulp 就决定进位方向，镜头数差一个。
    double weight_sum = 0.0;
    for (const auto& [d, w] : plan) {
        (void)d;
        weight_sum += w;
    }
    const double scale = 1.0 / weight_sum;

    std::map<double, int> slots;
    for (const auto& [dur, share] : plan) {
        slots[dur] = static_cast<int>(
            std::max<long>(0, py_round(target_s * share * scale / dur)));
    }

    // 用最长的档位补足或削减差额
    const double pad = all.back();
    slots[pad] = std::max(1, slots.count(pad) ? slots[pad] : 0);
    double sum = 0.0;
    for (const auto& [d, n] : slots) sum += d * n;
    const double diff = target_s - sum;
    slots[pad] = std::max<int>(1, slots[pad] + static_cast<int>(py_round(diff / pad)));

    DurationQuota q;
    for (const auto& [d, n] : slots) {
        if (n > 0) q.slots[d] = n;
    }
    return q;
}

double snap_duration(double seconds) {
    // min(..., key=...) 取**第一个**最小的，平局时靠前的赢。
    // 用严格小于就是这个语义。
    const std::vector<double>& slots = duration_slots();
    double best = slots.front();
    double best_gap = std::fabs(best - seconds);
    for (std::size_t i = 1; i < slots.size(); ++i) {
        const double gap = std::fabs(slots[i] - seconds);
        if (gap < best_gap) {
            best = slots[i];
            best_gap = gap;
        }
    }
    return best;
}

double ceil_duration(double seconds) {
    for (const double slot : duration_slots()) {
        if (slot >= seconds - 1e-6) return slot;
    }
    return duration_slots().back();
}

ordered llm_shot_schema(const AssetLibrary& assets) {
    ordered full = shot_schema_base();
    ordered props = full.contains("properties") ? full["properties"]
                                                : ordered::object();
    ordered defs = full.contains("$defs") ? full["$defs"] : ordered::object();

    ordered kept = ordered::object();
    for (const auto& item : props.items()) {
        if (llm_shot_fields().count(item.key())) kept[item.key()] = item.value();
    }

    const std::vector<std::string> char_ids = assets.character_ids();
    const std::vector<std::string> loc_ids = assets.location_ids();
    if (char_ids.empty()) {
        throw StoryboardError("资产库里一个角色都没有。请先生成角色圣经");
    }

    // 角色 id 收紧成枚举。这是防止模型凭空造角色最硬的手段。
    if (defs.contains("CharacterInShot")) {
        ordered& cis = defs["CharacterInShot"];
        if (!cis.contains("properties")) cis["properties"] = ordered::object();
        cis["properties"]["char_id"] = {
            {"type", "string"}, {"enum", char_ids},
            {"description", "必须是已注册角色之一"}};
    }
    if (defs.contains("DialogueLine")) {
        ordered& dl = defs["DialogueLine"];
        if (!dl.contains("properties")) dl["properties"] = ordered::object();
        dl["properties"]["char_id"] = {
            {"anyOf", ordered::array({ordered{{"type", "string"}, {"enum", char_ids}},
                                      ordered{{"type", "null"}}})},
            {"description", "说话角色。旁白留空"}};
        // 时长由配音阶段回填，不让模型猜
        for (const char* gone : {"audio_path", "actual_duration_s", "voice_id"}) {
            dl["properties"].erase(gone);
        }
    }

    if (!loc_ids.empty()) {
        kept["location_id"] = {
            {"anyOf", ordered::array({ordered{{"type", "string"}, {"enum", loc_ids}},
                                      ordered{{"type", "null"}}})}};
    }
    kept["duration_s"] = {{"type", "number"},
                          {"enum", duration_slots()},
                          {"description", "只能取这些值"}};

    // characters 和 dialogue 必须是必填并且带说明。
    // 只给一个 $ref 而不说要填什么，模型会整个略过这两个字段，
    // 结果是分镜里一句台词都没有，配音和口型全部落空。
    kept["characters"] = {
        {"type", "array"},
        {"items", ordered{{"$ref", "#/$defs/CharacterInShot"}}},
        {"description", "本镜出现的角色。没有人物出镜就填空数组"}};
    kept["dialogue"] = {
        {"type", "array"},
        {"items", ordered{{"$ref", "#/$defs/DialogueLine"}}},
        {"description",
         "本镜的台词和旁白，逐句填。剧本里的每一句话都必须落到某个镜头上，"
         "不能丢。这一镜没有人说话就填空数组"}};

    ordered shots_item = ordered::object();
    shots_item["type"] = "object";
    shots_item["properties"] = kept;
    shots_item["required"] = {"shot_id", "scene_id", "order", "first_frame_prompt",
                              "shot_size", "duration_s", "characters", "dialogue"};
    shots_item["additionalProperties"] = false;

    ordered shots = ordered::object();
    shots["type"] = "array";
    shots["items"] = shots_item;

    ordered out_props = ordered::object();
    out_props["shots"] = shots;

    ordered out = ordered::object();
    out["type"] = "object";
    out["properties"] = out_props;
    out["required"] = {"shots"};
    out["additionalProperties"] = false;
    out["$defs"] = defs;
    return out;
}

std::string build_storyboard_prompt(const std::string& script,
                                    const AssetLibrary& assets,
                                    const DurationQuota& quota,
                                    const std::string& episode_id) {
    std::vector<std::string> roster_lines;
    for (const std::string& cid : assets.character_ids()) {
        roster_lines.push_back("  " + cid + "：" + assets.characters.at(cid).name);
    }
    std::vector<std::string> place_lines;
    for (const std::string& lid : assets.location_ids()) {
        place_lines.push_back("  " + lid + "：" + assets.locations.at(lid).name);
    }
    const std::string roster = join_lines(roster_lines);
    // 对应 Python 的 "..." or "  （未定义场景，location_id 留空）"
    const std::string places = place_lines.empty()
                                   ? "  （未定义场景，location_id 留空）"
                                   : join_lines(place_lines);

    std::string out;
    out += prompt::kSbSeg0;
    out += roster;
    out += prompt::kSbSeg1;
    out += places;
    out += prompt::kSbSeg2;
    out += quota.describe();
    out += prompt::kSbSeg3;
    out += std::to_string(quota.shot_count());
    out += prompt::kSbSeg4;
    out += format_g(quota.total_s());
    out += prompt::kSbSeg5;
    out += episode_id;
    out += prompt::kSbSeg6;
    out += script;
    out += prompt::kSbSeg7;
    return out;
}

bool link_location(json& item, const std::set<std::string>& known) {
    if (truthy(item, "location_id")) return false;
    const std::string scene = str_or(item, "scene_id");
    if (known.count(scene)) {
        item["location_id"] = scene;
        return true;
    }
    return false;
}

void add_missing_speakers(json& item, const std::set<std::string>& known) {
    if (!item.is_object()) return;
    const auto dit = item.find("dialogue");
    if (dit == item.end() || !dit->is_array() || dit->empty()) return;

    if (!item.contains("characters") || !item["characters"].is_array()) {
        item["characters"] = json::array();
    }
    json& chars = item["characters"];

    std::set<std::string> present;
    for (const auto& c : chars) {
        if (c.is_object() && c.contains("char_id") && c["char_id"].is_string()) {
            present.insert(c["char_id"].get<std::string>());
        }
    }
    for (const auto& line : *dit) {
        if (!line.is_object()) continue;
        const auto sit = line.find("char_id");
        if (sit == line.end() || !sit->is_string()) continue;
        const std::string speaker = sit->get<std::string>();
        if (speaker.empty()) continue;
        if (known.count(speaker) && !present.count(speaker)) {
            chars.push_back(json{{"char_id", speaker}});
            present.insert(speaker);
        }
    }
}

std::vector<Shot> parse_storyboard(const std::string& raw,
                                   const AssetLibrary& assets) {
    json data;
    try {
        data = extract_json(raw);
    } catch (const std::exception& e) {
        throw StoryboardError(e.what());
    }

    json items;
    if (data.is_object()) {
        items = data.contains("shots") ? data["shots"] : json();
    } else {
        items = data;
    }
    if (!items.is_array()) {
        throw StoryboardError("大模型没有返回镜头列表");
    }
    if (items.empty()) throw StoryboardError("大模型返回了空的分镜表");

    std::set<std::string> known_chars;
    for (const auto& kv : assets.characters) known_chars.insert(kv.first);
    std::set<std::string> known_locs;
    for (const auto& kv : assets.locations) known_locs.insert(kv.first);

    std::vector<Shot> shots;
    std::vector<std::string> problems;
    for (std::size_t i = 0; i < items.size(); ++i) {
        json item = items[i];
        if (!item.is_object()) {
            problems.push_back("第 " + std::to_string(i + 1) + " 个镜头不是对象");
            continue;
        }

        if (!item.contains("order")) item["order"] = static_cast<int>(i);

        // float(x or 默认值)：0、null、缺失、空串都落到默认值
        double dur = duration_slots().back();
        if (truthy(item, "duration_s") && item["duration_s"].is_number()) {
            dur = item["duration_s"].get<double>();
        }
        item["duration_s"] = snap_duration(dur);

        // 模型常忘了硬切必须零时长，这里兜一下而不是报错退出
        if (str_or(item, "transition_in", "cut") == "cut") {
            item["transition_dur_s"] = 0.0;
        } else if (!truthy(item, "transition_dur_s")) {
            item["transition_dur_s"] = 0.4;
        }

        add_missing_speakers(item, known_chars);
        link_location(item, known_locs);

        try {
            Shot s = item.get<Shot>();
            const auto errs = s.validate();
            if (!errs.empty()) {
                throw std::runtime_error(errs.front());
            }
            shots.push_back(std::move(s));
        } catch (const std::exception& e) {
            problems.push_back("第 " + std::to_string(i + 1) + " 个镜头不合法：" +
                               e.what());
        }
    }

    if (!problems.empty()) {
        std::vector<std::string> first5(
            problems.begin(),
            problems.begin() + static_cast<long>(std::min<std::size_t>(5, problems.size())));
        throw StoryboardError("分镜表有 " + std::to_string(problems.size()) +
                              " 个镜头不合法：\n" + join_lines(first5));
    }

    // 先进 set 去重，再转 vector——validate_references 收的是 vector，
    // 而 Python 那边传的是集合推导式，重复的 id 只报一次。
    std::set<std::string> used_char_set, used_loc_set;
    for (const Shot& s : shots) {
        for (const auto& c : s.characters) used_char_set.insert(c.char_id);
        if (s.location_id.has_value() && !s.location_id->empty()) {
            used_loc_set.insert(*s.location_id);
        }
    }
    const std::vector<std::string> used_chars(used_char_set.begin(),
                                              used_char_set.end());
    const std::vector<std::string> used_locs(used_loc_set.begin(),
                                             used_loc_set.end());
    const auto ref_problems = assets.validate_references(used_chars, used_locs);
    if (!ref_problems.empty()) {
        throw StoryboardError("分镜引用了未注册的资产：\n" + join_lines(ref_problems));
    }
    return shots;
}

std::vector<std::string> check_coverage(const std::string& script,
                                        const std::vector<Shot>& shots) {
    // 对应 re.search(r"[：:]\s*\S", script)：找一个冒号，后面跳过空白，
    // 再要求至少一个非空白字符。手写而不用 std::regex——那东西在长输入上
    // 会递归到爆栈，而这里的输入是整集剧本。
    const auto is_ws = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
               c == '\v' || c == '\f';
    };
    bool script_has_dialogue = false;
    for (std::size_t i = 0; i < script.size() && !script_has_dialogue; ++i) {
        std::size_t after = 0;
        if (script[i] == ':') {
            after = i + 1;
        } else if (script.compare(i, 3, "：") == 0) {   // 全角冒号 U+FF1A
            after = i + 3;
        } else {
            continue;
        }
        while (after < script.size() &&
               is_ws(static_cast<unsigned char>(script[after]))) {
            ++after;
        }
        if (after < script.size()) script_has_dialogue = true;
    }

    std::vector<std::string> problems;
    std::size_t shot_lines = 0;
    for (const Shot& s : shots) shot_lines += s.dialogue.size();
    if (script_has_dialogue && shot_lines == 0) {
        problems.push_back(
            "剧本里有对白，但分镜表里一句台词都没有。"
            "模型多半漏填了 dialogue 字段");
    }
    const bool any_chars = std::any_of(
        shots.begin(), shots.end(),
        [](const Shot& s) { return !s.characters.empty(); });
    if (!any_chars) {
        problems.push_back(
            "所有镜头的 characters 都是空的，没有任何角色出镜。"
            "模型多半漏填了 characters 字段");
    }
    return problems;
}

std::vector<Shot>& rebalance_durations(std::vector<Shot>& shots, double target_s,
                                       double tolerance_s) {
    double current = 0.0;
    for (const Shot& s : shots) current += s.duration_s;
    if (std::fabs(current - target_s) <= tolerance_s) return shots;

    std::vector<Shot*> adjustable;
    for (Shot& s : shots) {
        if (s.dialogue.empty() && !s.duration_locked) adjustable.push_back(&s);
    }
    if (adjustable.empty()) return shots;

    const std::vector<double>& slots = duration_slots();
    double diff = target_s - current;
    const int step = diff > 0 ? 1 : -1;
    int guard = 0;
    while (std::fabs(diff) > tolerance_s && guard < 500) {
        ++guard;
        bool moved = false;
        for (Shot* shot : adjustable) {
            // 先吸附再查表。时长可能不在档位表里：老项目升级、用户手改分镜、
            // 或者档位表本身变过。直接查会越界。
            const double current_slot = snap_duration(shot->duration_s);
            const auto it = std::find(slots.begin(), slots.end(), current_slot);
            const long idx = static_cast<long>(std::distance(slots.begin(), it));
            const long new_idx = idx + step;
            if (new_idx < 0 || new_idx >= static_cast<long>(slots.size())) continue;
            const double delta = slots[static_cast<std::size_t>(new_idx)] -
                                 shot->duration_s;
            if (std::fabs(diff - delta) < std::fabs(diff)) {
                shot->duration_s = slots[static_cast<std::size_t>(new_idx)];
                diff -= delta;
                moved = true;
                if (std::fabs(diff) <= tolerance_s) break;
            }
        }
        if (!moved) break;
    }
    return shots;
}

}  // namespace changji::stages
