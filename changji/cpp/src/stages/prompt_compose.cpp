#include "stages/prompt_compose.hpp"

#include <map>

#include "util/text.hpp"

namespace changji::stages {

using namespace changji::models;

namespace {

const std::string kEmpty;

/// 表里查不到就返回空串。
///
/// 返回空串而不是抛异常，是照抄 Python 的 dict.get(k, "")。
/// 抛异常的话，加一个新枚举值忘了补表会让整条流水线挂掉；
/// 返回空串只是那一层少一个词。前者更响亮但也更容易在半夜炸掉一整晚的活。
template <typename K>
const std::string& lookup(const std::map<K, std::string>& m, K key) {
    const auto it = m.find(key);
    return it == m.end() ? kEmpty : it->second;
}

/// 用分隔符连接，**跳过空串**。
///
/// 跳空是必须的：不跳的话缺一层就多一个"，，"，
/// 而那个双逗号会被模型当成一个停顿信号。
std::string join_nonempty(const std::vector<std::string>& parts,
                          const std::string& sep) {
    std::string out;
    bool first = true;
    for (const auto& p : parts) {
        if (p.empty()) continue;
        if (!first) out += sep;
        out += p;
        first = false;
    }
    return out;
}

}  // namespace

const std::string& shot_size_zh(ShotSize v) {
    static const std::map<ShotSize, std::string> m = {
        {ShotSize::ECU, "大特写"}, {ShotSize::CU, "特写"},
        {ShotSize::MCU, "近景"},   {ShotSize::MS, "中景"},
        {ShotSize::MLS, "中远景"}, {ShotSize::LS, "远景"},
        {ShotSize::ELS, "大远景"},
    };
    return lookup(m, v);
}

const std::string& angle_zh(CameraAngle v) {
    static const std::map<CameraAngle, std::string> m = {
        {CameraAngle::LOW, "仰拍"},      {CameraAngle::EYE_LEVEL, "平视"},
        {CameraAngle::HIGH, "俯拍"},     {CameraAngle::OVERHEAD, "顶拍"},
        {CameraAngle::DUTCH, "斜角构图"},
    };
    return lookup(m, v);
}

const std::string& move_zh(CameraMove v) {
    static const std::map<CameraMove, std::string> m = {
        {CameraMove::STATIC, "固定镜头"},
        {CameraMove::PAN_LEFT, "向左横摇"},
        {CameraMove::PAN_RIGHT, "向右横摇"},
        {CameraMove::TILT_UP, "上摇"},
        {CameraMove::TILT_DOWN, "下摇"},
        {CameraMove::PUSH_IN, "镜头缓慢推近"},
        {CameraMove::PULL_OUT, "镜头缓慢拉远"},
        {CameraMove::HANDHELD, "手持轻微晃动"},
        {CameraMove::ORBIT, "环绕运镜"},
    };
    return lookup(m, v);
}

PromptComposer::PromptComposer(AssetLibrary assets)
    : assets_(std::move(assets)), style_line_(assets_.style.style_line) {
    // 动漫线是 Danbooru 标签串，用英文逗号加空格；写实线是自然语言，
    // 用中文逗号。两条线的分隔符不一样，混用会让标签串里冒出中文标点，
    // 而那对 Danbooru 词表训练出来的模型是噪声。
    sep_ = style_line_ == StyleLine::ANIME ? ", " : "，";
}

PromptBundle PromptComposer::compose(const Shot& shot) const {
    std::vector<std::string> layers;
    std::vector<std::string> refs;

    // ---- 身份层。逐字节从资产库拼出来，模型碰不到。----
    for (const CharacterInShot& in_shot : shot.characters) {
        const auto it = assets_.characters.find(in_shot.char_id);
        if (it == assets_.characters.end()) {
            throw RenderError("镜头 " + shot.shot_id + " 引用了未注册角色 " +
                              in_shot.char_id);
        }
        const Character& c = it->second;
        std::vector<std::string> beats = {
            c.render_prompt(style_line_, in_shot.wardrobe_state)};
        if (!in_shot.expression.empty()) beats.push_back(in_shot.expression);
        if (!in_shot.action.empty()) beats.push_back(in_shot.action);
        layers.push_back(join_nonempty(beats, sep_));

        const auto ref = c.ref_for_pose(to_string(in_shot.face_pose));
        if (ref.has_value() && !ref->empty()) refs.push_back(*ref);
    }

    // ---- 场景层 ----
    if (shot.location_id.has_value() && !shot.location_id->empty()) {
        const auto it = assets_.locations.find(*shot.location_id);
        if (it == assets_.locations.end()) {
            throw RenderError("镜头 " + shot.shot_id + " 引用了未注册场景 " +
                              *shot.location_id);
        }
        layers.push_back(it->second.render_prompt(style_line_));
        if (it->second.ref_empty.has_value() && !it->second.ref_empty->empty()) {
            refs.push_back(*it->second.ref_empty);
        }
    }

    // ---- 镜头层 ----
    layers.push_back(join_nonempty(
        {shot_size_zh(shot.shot_size), angle_zh(shot.camera_angle)}, sep_));
    if (!shot.first_frame_prompt.empty()) {
        layers.push_back(shot.first_frame_prompt);
    }

    // ---- 风格层 ----
    if (!assets_.style.global_style.empty()) {
        layers.push_back(assets_.style.global_style);
    }

    // 注意最后这一步过滤的是 **strip 之后为空**，不是原串为空。
    // 对应 Python 的 `if p.strip()`。一层里只有空格的话要整层丢掉，
    // 留着就是一个孤零零的分隔符。
    std::vector<std::string> kept;
    for (const auto& l : layers) {
        if (!text::strip_ws(l).empty()) kept.push_back(l);
    }

    PromptBundle out;
    out.positive = join_nonempty(kept, sep_);
    // 负向提示词是镜头自己的加上全剧的。镜头级的在前——
    // 它是针对这一镜的具体问题加的，权重该更高。
    out.negative = join_nonempty(
        {shot.negative_prompt, assets_.style.negative_prompt}, sep_);
    out.reference_images = std::move(refs);
    return out;
}

std::string PromptComposer::motion_prompt(const Shot& shot) const {
    std::vector<std::string> parts = {move_zh(shot.camera_move)};
    if (!shot.motion_prompt.empty()) parts.push_back(shot.motion_prompt);
    for (const CharacterInShot& in_shot : shot.characters) {
        if (!in_shot.action.empty()) parts.push_back(in_shot.action);
    }
    return join_nonempty(parts, sep_);
}

}  // namespace changji::stages
