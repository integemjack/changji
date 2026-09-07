#pragma once

// 分镜表：整套系统的中枢数据结构。
//
// 设计上最重要的一条：这里没有任何描述角色长相、发型、服装的字段。
//
// 保持角色跨镜头一致的常见做法是在提示词里反复强调，那不可靠。这里的做法是让
// 大模型在结构上就没有写错的机会：它只能填 char_id 和本镜可变项，外观描述在
// 渲染时由程序从角色资产库机械拼接，逐字节相同。
//
// 同一份定义既用于运行时校验，也用于导出 JSON Schema 约束大模型输出。
//
// 移植自 src/changji/models/shot.py。校验逻辑与那边的 pydantic 约束一一对应，
// 对拍时才能发现漏移植。

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "models/json_compat.hpp"

namespace changji::models {

// ── 枚举 ───────────────────────────────────────────────────────────────

/// 景别。取值顺序由近到远。
enum class ShotSize { ECU, CU, MCU, MS, MLS, LS, ELS };

enum class CameraAngle { LOW, EYE_LEVEL, HIGH, OVERHEAD, DUTCH };

enum class CameraMove {
    STATIC, PAN_LEFT, PAN_RIGHT, TILT_UP, TILT_DOWN,
    PUSH_IN, PULL_OUT, HANDHELD, ORBIT
};

/// 角色面部朝向。口型判定用。
enum class FacePose { FRONT, THREE_QUARTER, PROFILE, BACK, OFF_SCREEN };

enum class Transition { CUT, DISSOLVE, FADE_IN, FADE_OUT, WHIP };

/// 镜头在流水线上的位置。断点续跑靠它。
enum class ShotStatus {
    PLANNED,          ///< 分镜已出，未开工
    AUDIO_DONE,       ///< 配音已出，时长已锁
    FRAME_DONE,       ///< 首帧已出
    DRAFT_DONE,       ///< 草稿档视频已出
    DRAFT_REJECTED,   ///< 草稿未过闸门
    FINAL_DONE,       ///< 成片档已出
    FINAL_REJECTED,   ///< 成片未过闸门
    FALLBACK,         ///< 重试超限，降级为静帧加运镜
    LOCKED            ///< 人工确认，不再重跑
};

// 字符串取值必须与 Python 侧逐字相同，两个后端读写同一批项目文件。
NLOHMANN_JSON_SERIALIZE_ENUM(ShotSize, {
    {ShotSize::ECU, "ECU"}, {ShotSize::CU, "CU"}, {ShotSize::MCU, "MCU"},
    {ShotSize::MS, "MS"}, {ShotSize::MLS, "MLS"}, {ShotSize::LS, "LS"},
    {ShotSize::ELS, "ELS"},
})

NLOHMANN_JSON_SERIALIZE_ENUM(CameraAngle, {
    {CameraAngle::LOW, "low"}, {CameraAngle::EYE_LEVEL, "eye_level"},
    {CameraAngle::HIGH, "high"}, {CameraAngle::OVERHEAD, "overhead"},
    {CameraAngle::DUTCH, "dutch"},
})

NLOHMANN_JSON_SERIALIZE_ENUM(CameraMove, {
    {CameraMove::STATIC, "static"}, {CameraMove::PAN_LEFT, "pan_left"},
    {CameraMove::PAN_RIGHT, "pan_right"}, {CameraMove::TILT_UP, "tilt_up"},
    {CameraMove::TILT_DOWN, "tilt_down"}, {CameraMove::PUSH_IN, "push_in"},
    {CameraMove::PULL_OUT, "pull_out"}, {CameraMove::HANDHELD, "handheld"},
    {CameraMove::ORBIT, "orbit"},
})

NLOHMANN_JSON_SERIALIZE_ENUM(FacePose, {
    {FacePose::FRONT, "front"}, {FacePose::THREE_QUARTER, "three_quarter"},
    {FacePose::PROFILE, "profile"}, {FacePose::BACK, "back"},
    {FacePose::OFF_SCREEN, "off_screen"},
})

NLOHMANN_JSON_SERIALIZE_ENUM(Transition, {
    {Transition::CUT, "cut"}, {Transition::DISSOLVE, "dissolve"},
    {Transition::FADE_IN, "fade_in"}, {Transition::FADE_OUT, "fade_out"},
    {Transition::WHIP, "whip"},
})

NLOHMANN_JSON_SERIALIZE_ENUM(ShotStatus, {
    {ShotStatus::PLANNED, "planned"}, {ShotStatus::AUDIO_DONE, "audio_done"},
    {ShotStatus::FRAME_DONE, "frame_done"}, {ShotStatus::DRAFT_DONE, "draft_done"},
    {ShotStatus::DRAFT_REJECTED, "draft_rejected"},
    {ShotStatus::FINAL_DONE, "final_done"},
    {ShotStatus::FINAL_REJECTED, "final_rejected"},
    {ShotStatus::FALLBACK, "fallback"}, {ShotStatus::LOCKED, "locked"},
})

/// 枚举转回字符串。错误消息里要用，和 Python 的 `.value` 对齐。
const char* to_string(ShotSize v);
const char* to_string(CameraAngle v);
const char* to_string(CameraMove v);
const char* to_string(FacePose v);
const char* to_string(Transition v);
const char* to_string(ShotStatus v);

/// 能看清嘴部动作的景别。口型判定用，不要改成靠模型判断。
bool is_lipsync_capable(ShotSize v);
/// 俯拍和顶拍看不清嘴，排除在口型之外。
bool is_lipsync_capable(CameraAngle v);
bool is_lipsync_capable(FacePose v);

// ── 结构体 ─────────────────────────────────────────────────────────────

/// 角色在本镜头中的表现。
///
/// 只有可变项。外观描述属于角色资产，不在这里，也不允许大模型在这里写。
struct CharacterInShot {
    std::string char_id;                    ///< 角色 id，必须是已注册角色之一
    std::string expression;                 ///< 表情，如 愕然、隐忍（≤40）
    std::string action;                     ///< 本镜动作，如 后退半步（≤80）
    std::string wardrobe_state = "default"; ///< 服装状态 id，如 suit_torn（≤40）
    FacePose face_pose = FacePose::FRONT;   ///< 面部朝向
    std::string screen_pos = "center";      ///< 画面位置：left / center / right

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        CharacterInShot, char_id, expression, action, wardrobe_state,
        face_pose, screen_pos)

    void validate(const std::string& where, std::vector<std::string>& errs) const;
};

/// 一句台词。时长字段由配音阶段回填，分镜阶段不填。
struct DialogueLine {
    std::optional<std::string> char_id;  ///< 说话角色。为空表示旁白
    std::string text;                    ///< 1..200 字
    std::string emotion = "neutral";     ///< 情绪标签
    double emotion_intensity = 0.5;      ///< 0..1
    std::optional<std::string> voice_id; ///< 音色 id，由角色资产决定

    // 以下由配音阶段回填
    std::optional<std::string> audio_path;   ///< 相对项目根的路径
    std::optional<double> actual_duration_s; ///< ≥0

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        DialogueLine, char_id, text, emotion, emotion_intensity, voice_id,
        audio_path, actual_duration_s)

    void validate(const std::string& where, std::vector<std::string>& errs) const;
};

/// 一个镜头。
struct Shot {
    std::string shot_id;   ///< 全局唯一，如 ep01_s03_sh007；^[a-z0-9_]+$
    std::string scene_id;  ///< ^[a-z0-9_]+$
    int order = 0;         ///< 集内顺序，≥0

    // ---- 画面 ----
    std::string visual_desc;                      ///< 给人看的中文描述（≤300）
    std::string first_frame_prompt;               ///< ≤1200
    std::optional<std::string> last_frame_prompt; ///< 为空则走单帧图生视频（≤1200）
    std::string motion_prompt;                    ///< 运动描述，给视频模型（≤400）
    std::string negative_prompt;

    ShotSize    shot_size    = ShotSize::MS;
    CameraAngle camera_angle = CameraAngle::EYE_LEVEL;
    CameraMove  camera_move  = CameraMove::STATIC;
    std::optional<std::string> camera_id; ///< 复用机位 id。同场景同机位保证不越轴

    // ---- 引用（只放 id，不放描述）----
    std::vector<CharacterInShot> characters;
    std::optional<std::string> location_id;
    std::vector<std::string> prop_ids;

    // ---- 时间 ----
    double duration_s = 5.0;      ///< 镜头时长，(0, 30]
    bool duration_locked = false; ///< 真表示已由配音时长反推锁定，不可再调

    // ---- 声音 ----
    std::vector<DialogueLine> dialogue;
    std::vector<std::string> sfx;
    std::optional<std::string> bgm_cue;
    bool needs_lipsync = false; ///< 由 derive_needs_lipsync 规则推导，不要让模型填

    // ---- 剪辑 ----
    Transition transition_in = Transition::CUT;
    double transition_dur_s = 0.0; ///< 0..2
    std::string subtitle_text;

    // ---- 质控 ----
    std::string beat;             ///< 叙事功能，如 反转、铺垫（≤20）
    std::string continuity_notes; ///< ≤200
    std::vector<std::string> missing_info; ///< 模型自报的信息缺口，供校验阶段检查

    // ---- 运行时状态（大模型不填）----
    ShotStatus status = ShotStatus::PLANNED;
    int attempts = 0; ///< ≥0
    std::optional<std::string> frame_path;
    std::optional<std::string> video_path;
    std::vector<std::string> gate_notes;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        Shot, shot_id, scene_id, order,
        visual_desc, first_frame_prompt, last_frame_prompt, motion_prompt,
        negative_prompt, shot_size, camera_angle, camera_move, camera_id,
        characters, location_id, prop_ids,
        duration_s, duration_locked,
        dialogue, sfx, bgm_cue, needs_lipsync,
        transition_in, transition_dur_s, subtitle_text,
        beat, continuity_notes, missing_info,
        status, attempts, frame_path, video_path, gate_notes)

    /// 校验。返回错误列表而不是抛异常。
    ///
    /// 与 pydantic 的差异要说清楚：那边 model_validator 第一条不过就抛，
    /// 后面的条件看不到；这里把全部问题一次收齐。Web 层要把它们一次性回给
    /// 前端，逐条抛会让用户改一条报一条。对拍时只比较"有没有错"和错误内容，
    /// 不比较条数。
    std::vector<std::string> validate() const;

    bool has_onscreen_dialogue() const;

    /// 全部台词的实际总时长。任一句未配音则返回 nullopt。
    std::optional<double> total_dialogue_duration_s() const;
};

/// 判断一个镜头该不该做口型。
///
/// 这件事必须用规则算，不能交给大模型判断，它在这上面很不稳。
///
/// 四个条件同时成立才做：有出镜台词、景别够近能看清嘴、机位不是顶拍、
/// 并且说话的角色确实是正脸或侧脸对着镜头。
bool derive_needs_lipsync(const Shot& shot);

/// 批量回填 needs_lipsync。分镜生成后、配音之前调用。
void apply_lipsync_rules(std::vector<Shot>& shots);

}  // namespace changji::models
