#include "models/character.hpp"

#include "util/text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <set>
#include <tuple>

namespace changji::models {

namespace {

// rstrip_punct / strip_ws 搬去了 util/text.hpp——分镜阶段解析大模型输出时
// 也要用同一套清洗规则，两份拷贝迟早会分叉，而它们的输出进的是同一个提示词。
using text::rstrip_punct;
using text::strip_ws;

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::string sep_for(StyleLine line) {
    // 动漫线是 Danbooru 标签串，用英文逗号加空格；写实线是自然语言，用中文逗号。
    return line == StyleLine::ANIME ? ", " : "，";
}

std::string lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool has_prefix_slug(const std::string& s, const char* prefix) {
    const std::size_t n = std::char_traits<char>::length(prefix);
    if (s.size() <= n) return false;
    if (s.compare(0, n, prefix) != 0) return false;
    return std::all_of(s.begin() + static_cast<long>(n), s.end(),
                       [](unsigned char c) {
                           return (c >= 'a' && c <= 'z') ||
                                  (c >= '0' && c <= '9') || c == '_';
                       });
}

// 参考音色的偏好顺序。真正能用哪些由服务端说了算，
// 这里只是没得选时的默认倾向：中文样本优先，其次按性别分。
//
// 踩过的坑（Python 侧注释原样带过来）：写死 vibevoice 的中文样本提交上去，
// 节点校验直接拒绝，整条流水线断在配音这一步。文件在磁盘上，但不在这个
// 节点的列表里。参考音色在 ComfyUI 那边是个下拉框，装了哪些插件就有哪些
// 选项，换一台机器列表就不一样，所以不能假设任何一条路径一定存在。
const std::array<const char*, 6> kFemaleMarks = {
    "female", "woman", "_f_", "belinda", "mabel", "sophie"
};
const std::array<const char*, 7> kMaleMarks = {
    "male", "man", "_m_", "chadwick", "eastwood", "freeman", "attenborough"
};

const std::array<const char*, 10> kFemaleWords = {
    "女", "妈", "母", "姐", "妹", "婆", "娘", "妻", "太太", "阿姨"
};
const std::array<const char*, 10> kMaleWords = {
    "男", "爸", "父", "哥", "弟", "爷", "叔", "夫", "先生", "伯"
};

template <std::size_t N>
bool contains_any(const std::string& hay, const std::array<const char*, N>& needles) {
    return std::any_of(needles.begin(), needles.end(), [&](const char* n) {
        return hay.find(n) != std::string::npos;
    });
}

std::string voice_gender_of(const std::string& name) {
    const std::string low = lower_ascii(name);
    // female 里含 male，先判 female
    if (contains_any(low, kFemaleMarks)) return "female";
    if (contains_any(low, kMaleMarks)) return "male";
    return "";
}

}  // namespace

const char* to_string(StyleLine v) {
    return v == StyleLine::ANIME ? "anime" : "realistic";
}

// ── AppearanceBlock ────────────────────────────────────────────────────

std::string AppearanceBlock::render(StyleLine style_line) const {
    const std::array<const std::string*, 5> raw = {
        &identity, &body, &face, &attire, &style
    };
    std::vector<std::string> parts;
    parts.reserve(raw.size());
    for (const std::string* p : raw) {
        std::string cleaned = rstrip_punct(strip_ws(*p));
        if (!cleaned.empty()) parts.push_back(std::move(cleaned));
    }
    return join(parts, sep_for(style_line));
}

// ── Character ──────────────────────────────────────────────────────────

void Character::validate(std::vector<std::string>& errs) const {
    if (!has_prefix_slug(char_id, "c_")) {
        errs.push_back("角色 id 必须以 c_ 开头且只含小写字母数字下划线，当前是 " +
                       char_id);
    }
    if (name.empty()) {
        errs.push_back(char_id + "：name 不能为空");
    }
    if (appearance.identity.empty()) {
        errs.push_back(char_id + "：appearance.identity 不能为空");
    }
    if (appearance.face.empty()) {
        errs.push_back(char_id + "：appearance.face 不能为空");
    }
    if (appearance.attire.empty()) {
        errs.push_back(char_id + "：appearance.attire 不能为空");
    }
    if (lora_strength < 0.0 || lora_strength > 2.0) {
        errs.push_back(char_id + "：lora_strength 要在 0 到 2 之间");
    }
}

std::string Character::wardrobe_desc(const std::string& wardrobe_state) const {
    if (!wardrobe_state.empty() && wardrobe_state != "default") {
        for (const auto& v : wardrobe) {
            if (v.wardrobe_id == wardrobe_state) return v.description;
        }
    }
    return appearance.attire;
}

std::string Character::render_prompt(StyleLine style_line,
                                     const std::string& wardrobe_state) const {
    AppearanceBlock block = appearance;
    if (wardrobe_state != "default") {
        // 换装时只替换 attire 那一段，其余逐字节不变
        block.attire = wardrobe_desc(wardrobe_state);
    }
    std::string rendered = block.render(style_line);
    if (lora_trigger.has_value() && !lora_trigger->empty()) {
        rendered = *lora_trigger + sep_for(style_line) + rendered;
    }
    return rendered;
}

std::optional<std::string> Character::ref_for_pose(
    const std::string& face_pose) const {
    // 对应 Python 的那张字典：拿不到就逐级退回，最后退到正面图。
    if (face_pose == "front") return ref_front;
    if (face_pose == "three_quarter" || face_pose == "profile") {
        return ref_three_quarter.has_value() ? ref_three_quarter : ref_front;
    }
    if (face_pose == "back") {
        return ref_back.has_value() ? ref_back : ref_three_quarter;
    }
    // off_screen 和任何认不出来的取值都走 .get(face_pose, self.ref_front)
    return ref_front;
}

// ── Location ───────────────────────────────────────────────────────────

void Location::validate(std::vector<std::string>& errs) const {
    if (!has_prefix_slug(location_id, "loc_")) {
        errs.push_back("场景 id 必须以 loc_ 开头且只含小写字母数字下划线，当前是 " +
                       location_id);
    }
    if (name.empty()) errs.push_back(location_id + "：name 不能为空");
    if (space.empty()) errs.push_back(location_id + "：space 不能为空");
    if (lighting.empty()) errs.push_back(location_id + "：lighting 不能为空");
}

std::string Location::render_prompt(StyleLine style_line) const {
    // 注意：这里 Python 用的是 p.strip() 而不是 rstrip 标点，
    // 和 AppearanceBlock.render 不一样。不要"顺手统一"，
    // 输出必须逐字节对齐。
    std::vector<std::string> parts;
    for (const std::string* p : {&space, &lighting, &palette}) {
        std::string t = strip_ws(*p);
        if (!t.empty()) parts.push_back(std::move(t));
    }
    return join(parts, sep_for(style_line));
}

// ── AssetLibrary ───────────────────────────────────────────────────────

namespace {

/// 两个 JSON 类型共用的实现。
template <typename J>
void asset_library_to_json(J& j, const AssetLibrary& t) {
    j = J::object();
    j["characters"] = t.characters;
    j["locations"] = t.locations;
    j["style"] = J(nlohmann::json(t.style));
}

template <typename J>
void asset_library_from_json(const J& j, AssetLibrary& t) {
    const AssetLibrary def{};
    // 用 contains 而不是 at：缺字段要退回默认值，对应宏的 WITH_DEFAULT 语义。
    t.characters = j.contains("characters")
                       ? j.at("characters").template get<OrderedMap<Character>>()
                       : def.characters;
    t.locations = j.contains("locations")
                      ? j.at("locations").template get<OrderedMap<Location>>()
                      : def.locations;
    t.style = j.contains("style")
                  ? nlohmann::json(j.at("style")).template get<StyleProfile>()
                  : def.style;
}

}  // namespace

void to_json(nlohmann::json& j, const AssetLibrary& t) { asset_library_to_json(j, t); }
void from_json(const nlohmann::json& j, AssetLibrary& t) { asset_library_from_json(j, t); }
void to_json(nlohmann::ordered_json& j, const AssetLibrary& t) { asset_library_to_json(j, t); }
void from_json(const nlohmann::ordered_json& j, AssetLibrary& t) { asset_library_from_json(j, t); }


std::vector<std::string> AssetLibrary::character_ids() const {
    // 排序，对应 Python 的 sorted(self.characters)。
    // 注意这跟遍历顺序是两回事：遍历要插入顺序（响应里的数组），
    // 这里要字典序（给大模型做约束解码的枚举）。
    return characters.sorted_keys();
}

std::vector<std::string> AssetLibrary::location_ids() const {
    return locations.sorted_keys();
}

std::vector<std::string> AssetLibrary::validate_references(
    const std::vector<std::string>& char_ids,
    const std::vector<std::string>& loc_ids) const {
    std::vector<std::string> problems;
    // 去重加排序，对应 Python 的 sorted(char_ids - set(...))
    for (const auto& cid : std::set<std::string>(char_ids.begin(), char_ids.end())) {
        if (characters.find(cid) == characters.end()) {
            problems.push_back("角色 " + cid + " 未在资产库注册");
        }
    }
    for (const auto& lid : std::set<std::string>(loc_ids.begin(), loc_ids.end())) {
        if (locations.find(lid) == locations.end()) {
            problems.push_back("场景 " + lid + " 未在资产库注册");
        }
    }
    return problems;
}

std::vector<std::string> AssetLibrary::validate() const {
    std::vector<std::string> errs;
    for (const auto& kv : characters) {
        if (kv.first != kv.second.char_id) {
            errs.push_back("资产库的键 " + kv.first + " 与角色自己的 id " +
                           kv.second.char_id + " 不一致");
        }
        kv.second.validate(errs);
    }
    for (const auto& kv : locations) {
        if (kv.first != kv.second.location_id) {
            errs.push_back("资产库的键 " + kv.first + " 与场景自己的 id " +
                           kv.second.location_id + " 不一致");
        }
        kv.second.validate(errs);
    }
    return errs;
}

// ── 配音辅助 ───────────────────────────────────────────────────────────

std::string guess_gender(const std::string& identity) {
    const bool female = contains_any(identity, kFemaleWords);
    const bool male = contains_any(identity, kMaleWords);
    if (female && !male) return "female";
    if (male && !female) return "male";
    return "";
}

std::optional<std::string> pick_voice(const std::vector<std::string>& available,
                                      const std::string& gender, int index) {
    std::vector<std::string> pool;
    for (const auto& v : available) {
        if (!v.empty() && v != "none") pool.push_back(v);
    }
    if (pool.empty()) return std::nullopt;

    // 对应 Python 的 rank()：(性别档, 中文档, 名字本身)
    const auto rank = [&](const std::string& v) {
        const std::string vg = voice_gender_of(v);
        int gender_rank;
        if (gender.empty() || vg.empty()) {
            gender_rank = 1;              // 分不出来的排中间
        } else if (vg == gender) {
            gender_rank = 0;
        } else {
            gender_rank = 2;              // 性别相反的排最后
        }
        const std::string low = lower_ascii(v);
        const int zh_rank =
            (low.find("zh_") != std::string::npos ||
             low.find("zh-") != std::string::npos) ? 0 : 1;
        return std::make_tuple(gender_rank, zh_rank, v);
    };

    std::sort(pool.begin(), pool.end(), [&](const std::string& a, const std::string& b) {
        return rank(a) < rank(b);
    });

    const auto best = rank(pool.front());
    const int best_gender = std::get<0>(best);
    const int best_zh = std::get<1>(best);

    std::vector<std::string> tier;
    for (const auto& v : pool) {
        const auto r = rank(v);
        if (std::get<0>(r) == best_gender && std::get<1>(r) == best_zh) {
            tier.push_back(v);
        }
    }
    // 同一档里按角色序号轮着分，不同角色至少声音不同。
    // Python 的 % 对负数返回非负，C++ 不是，所以先归一化。
    const int n = static_cast<int>(tier.size());
    int i = index % n;
    if (i < 0) i += n;
    return tier[static_cast<std::size_t>(i)];
}

}  // namespace changji::models
