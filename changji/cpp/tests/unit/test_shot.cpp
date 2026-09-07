// Shot 模型的对拍测试。
//
// 语料全部由 Python 侧真实的 pydantic 模型导出（见 ../export_golden.py），
// 不是手写的。手写 JSON 只能测出写的人对字段的理解，测不出真实格式。
//
// 阶段 1 的完成标志是"能读 Python 写的项目文件，字段全部对得上"，
// 这个文件就是那句话的可执行版本。

#include <doctest/doctest.h>

#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "models/shot.hpp"

using namespace changji::models;
using json = nlohmann::json;

namespace {

json load_golden(const std::string& name) {
    const std::string path = std::string(CHANGJI_GOLDEN_DIR) + "/" + name + ".json";
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "读不到语料 " << path
                    << "（先跑 cpp/tests/export_golden.py）");
    json j;
    in >> j;
    return j;
}

}  // namespace

TEST_CASE("只有必填字段时，默认值和 pydantic 一致") {
    const json j = load_golden("shot_minimal");
    const Shot s = j.get<Shot>();

    CHECK(s.shot_id == "ep01_s01_sh001");
    CHECK(s.scene_id == "ep01_s01");
    CHECK(s.order == 0);

    // 这些默认值一旦和 Python 侧漂移，读一个省略了字段的项目文件时会静默
    // 拿到不同的值，而且不报错。所以逐个钉住。
    CHECK(s.shot_size == ShotSize::MS);
    CHECK(s.camera_angle == CameraAngle::EYE_LEVEL);
    CHECK(s.camera_move == CameraMove::STATIC);
    CHECK(s.transition_in == Transition::CUT);
    CHECK(s.status == ShotStatus::PLANNED);
    CHECK(s.duration_s == doctest::Approx(5.0));
    CHECK(s.transition_dur_s == doctest::Approx(0.0));
    CHECK(s.duration_locked == false);
    CHECK(s.needs_lipsync == false);
    CHECK(s.attempts == 0);

    CHECK_FALSE(s.last_frame_prompt.has_value());
    CHECK_FALSE(s.camera_id.has_value());
    CHECK_FALSE(s.location_id.has_value());
    CHECK_FALSE(s.bgm_cue.has_value());
    CHECK_FALSE(s.frame_path.has_value());
    CHECK_FALSE(s.video_path.has_value());

    CHECK(s.characters.empty());
    CHECK(s.dialogue.empty());
    CHECK(s.prop_ids.empty());
    CHECK(s.sfx.empty());
    CHECK(s.missing_info.empty());
    CHECK(s.gate_notes.empty());

    CHECK(s.validate().empty());
}

TEST_CASE("字段填满的中文镜头能原样读回") {
    const json j = load_golden("shot_full");
    const Shot s = j.get<Shot>();

    CHECK(s.shot_id == "ep01_s03_sh007");
    CHECK(s.shot_size == ShotSize::MCU);
    CHECK(s.camera_angle == CameraAngle::LOW);
    CHECK(s.camera_move == CameraMove::PUSH_IN);
    CHECK(s.transition_in == Transition::DISSOLVE);
    CHECK(s.transition_dur_s == doctest::Approx(0.8));
    CHECK(s.status == ShotStatus::AUDIO_DONE);
    CHECK(s.attempts == 2);
    CHECK(s.duration_locked);

    REQUIRE(s.characters.size() == 2);
    CHECK(s.characters[0].char_id == "lin_yuan");
    CHECK(s.characters[0].face_pose == FacePose::THREE_QUARTER);
    CHECK(s.characters[0].wardrobe_state == "suit_soaked");
    CHECK(s.characters[1].face_pose == FacePose::PROFILE);
    // 第二个角色没填 wardrobe_state，应该拿到默认值而不是空串
    CHECK(s.characters[1].wardrobe_state == "default");

    REQUIRE(s.dialogue.size() == 2);
    REQUIRE(s.dialogue[0].char_id.has_value());
    CHECK(*s.dialogue[0].char_id == "lin_yuan");
    CHECK(s.dialogue[0].emotion_intensity == doctest::Approx(0.8));
    REQUIRE(s.dialogue[0].actual_duration_s.has_value());
    CHECK(*s.dialogue[0].actual_duration_s == doctest::Approx(3.2));
    // 第二句是旁白，char_id 为空
    CHECK_FALSE(s.dialogue[1].char_id.has_value());
    CHECK_FALSE(s.dialogue[1].actual_duration_s.has_value());

    REQUIRE(s.video_path.has_value() == false);
    REQUIRE(s.frame_path.has_value());
    CHECK(*s.frame_path == "frames/ep01_s03_sh007.png");

    CHECK(s.validate().empty());

    SUBCASE("往返之后与 Python 写出来的深度相等") {
        // 契约标准是结构兼容不是逐字节，所以比较 json 对象而不是文本。
        // nlohmann 的 operator== 是顺序无关的深比较，正好对应那条标准。
        const json round_tripped = s;
        CHECK(round_tripped == j);
    }
}

TEST_CASE("有台词但未配音时总时长为空") {
    const Shot s = load_golden("shot_full").get<Shot>();
    // 第二句旁白没有 actual_duration_s，所以整体应该是 nullopt
    CHECK_FALSE(s.total_dialogue_duration_s().has_value());
    CHECK(s.has_onscreen_dialogue());
}

TEST_CASE("口型判定与 Python 的真值表逐行一致") {
    const json table = load_golden("lipsync_truth_table");
    REQUIRE(table.is_array());
    REQUIRE(table.size() >= 10);

    for (const auto& entry : table) {
        const std::string name = entry.at("name").get<std::string>();
        const Shot s = entry.at("shot").get<Shot>();
        const bool expected = entry.at("expected_needs_lipsync").get<bool>();

        CAPTURE(name);
        CHECK(derive_needs_lipsync(s) == expected);
    }
}

TEST_CASE("Python 拒绝的数据这边也要拒绝") {
    const json cases = load_golden("shot_invalid");
    REQUIRE(cases.is_array());

    for (const auto& item : cases) {
        const std::string why = item.at("why").get<std::string>();
        CAPTURE(why);

        // 这些 JSON 构造不出合法的 Shot，但反序列化本身不校验——
        // 校验是 validate() 的事。这正是与 pydantic 的结构差异：
        // 那边构造即校验，这边分两步。
        const Shot s = item.at("json").get<Shot>();
        const auto errs = s.validate();
        CHECK_MESSAGE(!errs.empty(), "这条本该被拒绝：" << why);
    }
}

TEST_CASE("300 个汉字必须通过，按字节数判会误拒") {
    // 和语料里那条 301 字的配对。visual_desc 的上限是 300 **字**，
    // 300 个汉字是 900 字节——用 std::string::size() 判会在这里失败。
    const Shot s = load_golden("shot_boundary_300_chars").get<Shot>();
    const auto errs = s.validate();
    for (const auto& e : errs) {
        INFO("意外的错误：" << e);
    }
    CHECK(errs.empty());
}

TEST_CASE("批量回填口型标记") {
    std::vector<Shot> shots;
    for (const auto& entry : load_golden("lipsync_truth_table")) {
        shots.push_back(entry.at("shot").get<Shot>());
    }
    for (auto& s : shots) s.needs_lipsync = false;

    apply_lipsync_rules(shots);

    const json table = load_golden("lipsync_truth_table");
    for (std::size_t i = 0; i < shots.size(); ++i) {
        CAPTURE(table[i].at("name").get<std::string>());
        CHECK(shots[i].needs_lipsync ==
              table[i].at("expected_needs_lipsync").get<bool>());
    }
}
