// 分镜阶段的对拍测试。
//
// 语料由 tests/export_storyboard_golden.py 调**真实的 Python 函数**生成。
//
// 这一阶段比角色圣经难对，因为多了浮点。配额分配里有三次 round()——
// Python 的 round() 是银行家舍入不是四舍五入，而且权重求和的顺序会
// 影响一个 ulp，那个 ulp 正好能决定卡在半整数上的进位方向。
// 差一个镜头就意味着提示词里的配额和实际不符。

#include <doctest/doctest.h>

#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "models/character.hpp"
#include "models/shot.hpp"
// 落位那几条要估台词念多久
#include "stages/audio_plan.hpp"
#include "stages/limits.hpp"
// frames_for：帧数的格子跟着模型走
#include "stages/render.hpp"
#include "stages/storyboard.hpp"

using namespace changji;
using json = nlohmann::json;

namespace {

const json& golden() {
    static const json g = [] {
        const std::string path =
            std::string(CHANGJI_GOLDEN_DIR) + "/stage_storyboard.json";
        std::ifstream in(path, std::ios::binary);
        REQUIRE_MESSAGE(in.good(), "读不到语料 " << path);
        json j;
        in >> j;
        return j;
    }();
    return g;
}

/// 和语料里那份一模一样的资产库。
models::AssetLibrary test_assets(bool with_locations = true) {
    models::AssetLibrary a;

    models::Character lin;
    lin.char_id = "c_lin_wan";
    lin.name = "林晚";
    lin.voice_order = 0;
    lin.appearance.identity = "二十七岁女性";
    lin.appearance.body = "偏瘦";
    lin.appearance.face = "黑色长直发，杏眼";
    lin.appearance.attire = "白色衬衫";
    a.characters["c_lin_wan"] = lin;

    models::Character chen;
    chen.char_id = "c_chen_mo";
    chen.name = "陈默";
    chen.voice_order = 1;
    chen.appearance.identity = "三十出头男性";
    chen.appearance.body = "高瘦";
    chen.appearance.face = "短寸黑发，浓眉";
    chen.appearance.attire = "黑色风衣";
    a.characters["c_chen_mo"] = chen;

    if (with_locations) {
        models::Location loc;
        loc.location_id = "loc_rooftop";
        loc.name = "夜间天台";
        loc.space = "水泥地面，锈蚀护栏";
        loc.lighting = "夜间冷调顶光";
        a.locations["loc_rooftop"] = loc;
    }

    a.style.global_style = "电影感，冷色调";
    return a;
}

}  // namespace

TEST_CASE("时长档位由 MAX_FRAMES 推导") {
    const json& d = golden().at("durations");
    // 5 秒是上限，8 和 10 那两档必须不在里面——早先它们在，
    // 超出的部分被静默截断，成片比计划短了一大截才被闸门发现。
    CHECK(stages::duration_slots() == d.at("slots").get<std::vector<double>>());
    CHECK(stages::max_shot_duration_s() ==
          doctest::Approx(d.at("max_shot_duration_s").get<double>()));
    CHECK(stages::max_shot_duration_s() == doctest::Approx(121.0 / 24.0));
}

TEST_CASE("时长吸附") {
    for (const auto& c : golden().at("durations").at("snap")) {
        const double in = c.at("in").get<double>();
        CAPTURE(in);
        CHECK(stages::snap_duration(in) == c.at("out").get<double>());
    }
    // 2.5 到 2 和到 3 一样远。min() 取第一个最小的，所以是 2。
    // 用 <= 而不是 < 会得到 3，两边就对不上了。
    CHECK(stages::snap_duration(2.5) == 2.0);
    CHECK(stages::snap_duration(3.5) == 3.0);
}

TEST_CASE("向上吸附") {
    for (const auto& c : golden().at("durations").at("ceil")) {
        const double in = c.at("in").get<double>();
        CAPTURE(in);
        CHECK(stages::ceil_duration(in) == c.at("out").get<double>());
    }
    // 2.0000001 这种浮点毛刺不该跳档。配音时长反推镜头时长时经常撞上。
    CHECK(stages::ceil_duration(2.0 + 1e-9) == 2.0);
}

TEST_CASE("配额分配和 Python 一致") {
    for (const auto& c : golden().at("quotas")) {
        const double target = c.at("target_s").get<double>();
        CAPTURE(target);
        const stages::DurationQuota q =
            stages::DurationQuota::for_duration(target);

        json got_slots = json::array();
        for (const auto& [d, n] : q.slots) got_slots.push_back({d, n});
        if (got_slots != c.at("slots")) {
            MESSAGE("期望 " << c.at("slots").dump());
            MESSAGE("实得 " << got_slots.dump());
        }
        CHECK(got_slots == c.at("slots"));
        CHECK(q.total_s() == c.at("total_s").get<double>());
        CHECK(q.shot_count() == c.at("shot_count").get<int>());
        CHECK(q.describe() == c.at("describe").get<std::string>());
    }
}

TEST_CASE("目标时长必须为正") {
    CHECK_THROWS_AS(stages::DurationQuota::for_duration(0.0),
                    stages::StoryboardError);
    CHECK_THROWS_AS(stages::DurationQuota::for_duration(-1.0),
                    stages::StoryboardError);
}

TEST_CASE("配额描述里的时长不带小数点") {
    // "3 个 5 秒镜头" 不是 "3 个 5.0 秒镜头"。Python 用的是 f"{d:g}"。
    // 这句话进提示词，多一个 .0 就破契约了。
    stages::DurationQuota q;
    q.slots[2.0] = 3;
    q.slots[5.0] = 1;
    CHECK(q.describe() == "3 个 2 秒镜头，1 个 5 秒镜头");

    SUBCASE("数量为零的档位不出现在描述里") {
        q.slots[4.0] = 0;
        CHECK(q.describe() == "3 个 2 秒镜头，1 个 5 秒镜头");
        // 但它仍然算进 total_s（0 个 4 秒等于 0 秒，不影响）
        CHECK(q.total_s() == 11.0);
        CHECK(q.shot_count() == 4);
    }
}

TEST_CASE("提示词和 Python 逐字节一致") {
    for (const auto& c : golden().at("prompts")) {
        const std::string ep = c.at("episode_id").get<std::string>();
        CAPTURE(ep);
        const bool no_loc = c.value("no_locations", false);
        const models::AssetLibrary a = test_assets(!no_loc);
        const stages::DurationQuota q =
            stages::DurationQuota::for_duration(c.at("target_s").get<double>());

        const std::string got = stages::build_storyboard_prompt(
            c.at("script").get<std::string>(), a, q, ep);
        const std::string want = c.at("prompt").get<std::string>();
        if (got != want) {
            std::size_t i = 0;
            while (i < got.size() && i < want.size() && got[i] == want[i]) ++i;
            MESSAGE("第一处不同在字节 " << i);
            MESSAGE("期望…" << want.substr(i > 40 ? i - 40 : 0, 90));
            MESSAGE("实得…" << got.substr(i > 40 ? i - 40 : 0, 90));
        }
        CHECK(got == want);
    }
}

TEST_CASE("没有场景时提示词里有那句占位") {
    const models::AssetLibrary a = test_assets(/*with_locations=*/false);
    const auto q = stages::DurationQuota::for_duration(60.0);
    const std::string p = stages::build_storyboard_prompt("剧本", a, q, "ep_01");
    // 空着的话模型会以为场景列表被截断了，然后自己编一个 id
    CHECK(p.find("（未定义场景，location_id 留空）") != std::string::npos);
}

TEST_CASE("给大模型的 schema 和 Python 一致") {
    // 字段名错一个，模型就按错的名字产出，解析阶段拿到一堆空值——
    // 不报错，跑完一整集才发现。
    for (const auto& c : golden().at("schemas")) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);
        const models::AssetLibrary a = test_assets(name == "带场景");
        const json got = json(stages::llm_shot_schema(a));
        if (got != c.at("schema")) {
            // 整份 dump 太长，先报哪些顶层键对不上
            for (const auto& item : c.at("schema").items()) {
                if (!got.contains(item.key()) || got[item.key()] != item.value()) {
                    MESSAGE("顶层键对不上：" << item.key());
                }
            }
        }
        CHECK(got == c.at("schema"));
    }
}

TEST_CASE("schema 里角色 id 被收紧成枚举") {
    // 这是防止模型凭空造角色最硬的手段，单独钉一下
    const json s = json(stages::llm_shot_schema(test_assets()));
    const json& cis = s.at("$defs").at("CharacterInShot").at("properties").at("char_id");
    CHECK(cis.at("enum") == json::array({"c_chen_mo", "c_lin_wan"}));

    // 时长也只能取档位里的值
    CHECK(s.at("properties").at("shots").at("items").at("properties")
           .at("duration_s").at("enum") ==
          json(stages::duration_slots()));

    // 配音阶段回填的三个字段不能让模型猜
    const json& dl = s.at("$defs").at("DialogueLine").at("properties");
    CHECK_FALSE(dl.contains("audio_path"));
    CHECK_FALSE(dl.contains("actual_duration_s"));
    CHECK_FALSE(dl.contains("voice_id"));

    // needs_lipsync 由规则算，不在允许字段里
    CHECK_FALSE(s.at("properties").at("shots").at("items")
                 .at("properties").contains("needs_lipsync"));
}

TEST_CASE("运动那两栏是必填的") {
    // **上面那条对拍测不出这个。** 它比的是整份 schema 相等，而语料是跟着
    // 代码一起改的；真正要钉死的是「为什么」，所以单拎出来一条。
    //
    // 依据是一份真实项目：雨夜天台 198 镜里 motion_prompt 空了 198 个、
    // camera_move 是 static 的 198 个——恰好都是 Shot 结构体的默认值，
    // 而同一张表里在 required 里的 shot_size 有五种取值。
    // 不在 required 里 = 模型整个略过 = 图生视频只收到「固定镜头，站立不动」。
    const json s = json(stages::llm_shot_schema(test_assets()));
    const json& item = s.at("properties").at("shots").at("items");
    const json& req = item.at("required");
    const auto required_has = [&req](const char* key) {
        for (const auto& v : req) {
            if (v == key) return true;
        }
        return false;
    };

    CHECK(required_has("motion_prompt"));
    CHECK(required_has("camera_move"));

    const json& motion = item.at("properties").at("motion_prompt");
    // 光必填还不够——填一个字也算填了。
    CHECK(motion.at("minLength") == 12);
    // 下限别再往上抬：高过这一镜真有的内容时，模型会拿 JSON 字段名凑数。
    CHECK(motion.at("maxLength") == 400);
    // default 留着等于告诉模型「这一栏可以不管」。
    CHECK_FALSE(motion.contains("default"));

    const json& move = item.at("properties").at("camera_move");
    CHECK_FALSE(move.contains("default"));
    // 枚举跟着 Shot 的定义走，不在 storyboard.cpp 里抄第二份：
    // 那边加一种运镜、这边忘了跟，模型就永远挑不到新的那个。
    CHECK(move.at("enum") ==
          s.at("$defs").at("CameraMove").at("enum"));
    CHECK(move.at("enum").size() == 9);
}

TEST_CASE("资产库没有角色时报错") {
    models::AssetLibrary empty;
    CHECK_THROWS_AS(stages::llm_shot_schema(empty), stages::StoryboardError);
}

TEST_CASE("scene_id 接到 location_id 上") {
    const std::set<std::string> known = {"loc_rooftop", "loc_office"};
    for (const auto& c : golden().at("links")) {
        json item = c.at("before");
        CAPTURE(item.dump());
        const bool changed = stages::link_location(item, known);
        CHECK(changed == c.at("changed").get<bool>());
        CHECK(item == c.at("after"));
    }
}

TEST_CASE("漏填的说话人被补进角色列表") {
    const std::set<std::string> known = {"c_lin_wan", "c_chen_mo"};
    for (const auto& c : golden().at("speakers")) {
        json item = c.at("before");
        CAPTURE(item.dump());
        stages::add_missing_speakers(item, known);
        if (item != c.at("after")) {
            MESSAGE("期望 " << c.at("after").dump());
            MESSAGE("实得 " << item.dump());
        }
        CHECK(item == c.at("after"));
    }
}

TEST_CASE("解析出的镜头和 Python 一致") {
    const models::AssetLibrary a = test_assets();
    for (const auto& c : golden().at("parses")) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);
        const std::vector<models::Shot> shots =
            stages::parse_storyboard(c.at("raw").get<std::string>(), a);

        json got = json::array();
        for (const models::Shot& s : shots) got.push_back(s);
        if (got != c.at("shots")) {
            MESSAGE("期望 " << c.at("shots").dump(1));
            MESSAGE("实得 " << got.dump(1));
        }
        CHECK(got == c.at("shots"));
    }
}

TEST_CASE("解析时的三处兜底") {
    // 这三条都是"模型没填对但不该让整条流水线挂掉"的情况。
    // 语料里那份分镜刻意留了这三个坑。
    const models::AssetLibrary a = test_assets();
    const json& c = golden().at("parses").at(0);
    const std::vector<models::Shot> shots =
        stages::parse_storyboard(c.at("raw").get<std::string>(), a);
    REQUIRE(shots.size() == 3);

    // 一，第二个镜头没填 order，按下标补 1
    CHECK(shots[1].order == 1);

    // 二，第三个镜头 duration_s 是 4.4，不在档位上，被吸附到 4
    CHECK(shots[2].duration_s == 4.0);

    // 三，第二个镜头的说话人没进 characters，被补进去
    REQUIRE(shots[1].characters.size() == 1);
    CHECK(shots[1].characters[0].char_id == "c_lin_wan");

    // 另外：scene_id 是已注册场景，location_id 空着，被接上
    REQUIRE(shots[1].location_id.has_value());
    CHECK(*shots[1].location_id == "loc_rooftop");

    // 硬切必须零时长；dissolve 忘填时长补 0.4
    CHECK(shots[0].transition_dur_s == 0.0);
    CHECK(shots[2].transition_in == models::Transition::DISSOLVE);
    CHECK(shots[2].transition_dur_s == doctest::Approx(0.4));
}

TEST_CASE("该报错的都报错") {
    const models::AssetLibrary a = test_assets();
    for (const auto& c : golden().at("parse_failures")) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);
        const std::string raw = c.at("raw").get<std::string>();
        if (c.at("raises").get<bool>()) {
            CHECK_THROWS_AS(stages::parse_storyboard(raw, a),
                            stages::StoryboardError);
        } else {
            CHECK_NOTHROW(stages::parse_storyboard(raw, a));
        }
    }
}

TEST_CASE("覆盖检查") {
    const models::AssetLibrary a = test_assets();
    for (const auto& c : golden().at("coverages")) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);

        std::vector<models::Shot> shots;
        for (const auto& s : c.at("shots")) shots.push_back(s.get<models::Shot>());

        const auto got = stages::check_coverage(
            c.at("script").get<std::string>(), shots);
        CHECK(got == c.at("problems").get<std::vector<std::string>>());
    }
}

TEST_CASE("认不出的枚举取值当没填，不能变成表里第一项") {
    // nlohmann 的枚举反序列化认不出取值时会静默回落到表里第一项。客户端
    // 那条路早就回头验过一次（editing.cpp 的 parse_enum，对拍语料里
    // `shot_size: "XXL"` 是一条 400），模型这条路一直照单收。
    //
    // 最刺眼的是 shot_size：缺这个键给的是 MS（中景），填「medium」给的
    // 却是 ECU（**大特写**）——同一件事两种结果，而且错的那种更离谱。
    const models::AssetLibrary a = test_assets();
    json doc = json::parse(golden().at("parses").at(0).at("raw").get<std::string>());
    json& items = (doc.is_object() && doc.contains("shots")) ? doc["shots"] : doc;
    REQUIRE(items.is_array());
    REQUIRE_FALSE(items.empty());
    for (auto& sh : items) {
        sh["shot_size"] = "medium";        // 认不出 → 原来会变成 ECU
        sh["camera_angle"] = "45deg";      // 认不出 → 原来会变成 low
        sh["camera_move"] = "zoom";
        sh["lens"] = "35mm";
        sh["transition_in"] = "wipe";
        if (sh.contains("characters") && sh["characters"].is_array()) {
            for (auto& c : sh["characters"]) {
                if (c.is_object()) c["face_pose"] = "side";
            }
        }
    }

    const std::vector<models::Shot> shots = stages::parse_storyboard(doc.dump(), a);
    REQUIRE_FALSE(shots.empty());
    bool saw_character = false;
    for (const models::Shot& s : shots) {
        CHECK(s.shot_size == models::ShotSize::MS);
        CHECK(s.camera_angle == models::CameraAngle::EYE_LEVEL);
        CHECK(s.camera_move == models::CameraMove::STATIC);
        CHECK(s.lens == models::Lens::AUTO);
        CHECK(s.transition_in == models::Transition::CUT);
        for (const models::CharacterInShot& c : s.characters) {
            saw_character = true;
            CHECK(c.face_pose == models::FacePose::FRONT);
        }
    }
    CHECK(saw_character);

    // 认得出的取值要原样留着——别把这一条写成"所有枚举都清零"
    for (auto& sh : items) sh["shot_size"] = "ELS";
    const std::vector<models::Shot> kept = stages::parse_storyboard(doc.dump(), a);
    REQUIRE_FALSE(kept.empty());
    for (const models::Shot& s : kept) CHECK(s.shot_size == models::ShotSize::ELS);
}

TEST_CASE("模型多填的状态字段一律剥掉") {
    // schema 那边已经把字段裁到 kLlmShotFields 了，但 schema 走的是各家
    // provider 的 response_format——支持得好不好各不相同，不支持的那几家
    // 它只是提示词里的一段字。所以**解析这一头也要拦一道**。
    //
    // 最重的是 status：填成 final_done / locked，出片那一步整镜跳过
    // （这两个不在 render_entry_states 里），表现是"跑完了，这一镜什么都
    // 没出"，而侧边栏还会把这一集打上勾。frame_path / video_path 会让装配
    // 去拼一个不存在的文件；台词行里的 voice_id 会让这一句换个人说话。
    // 这一类全都不报错。
    const models::AssetLibrary a = test_assets();
    json doc = json::parse(golden().at("parses").at(0).at("raw").get<std::string>());
    json& items = (doc.is_object() && doc.contains("shots")) ? doc["shots"] : doc;
    REQUIRE(items.is_array());
    REQUIRE_FALSE(items.empty());
    for (auto& sh : items) {
        sh["status"] = "final_done";
        sh["frame_path"] = "frames/编出来的.png";
        sh["video_path"] = "shots/编出来的.mp4";
        sh["attempts"] = 7;
        sh["gate_notes"] = json::array({"模型自己写的一条"});
        sh["duration_locked"] = true;
        sh["needs_lipsync"] = true;
        sh["negative_prompt"] = "模型自己加的负向";
        if (sh.contains("dialogue") && sh["dialogue"].is_array()) {
            for (auto& dl : sh["dialogue"]) {
                dl["voice_id"] = "编出来的音色";
                dl["audio_path"] = "audio/编出来的.wav";
                dl["actual_duration_s"] = 99.0;
            }
        }
    }

    const std::vector<models::Shot> shots = stages::parse_storyboard(doc.dump(), a);
    REQUIRE_FALSE(shots.empty());
    bool saw_dialogue = false;
    for (const models::Shot& s : shots) {
        CHECK(s.status == models::ShotStatus::PLANNED);
        CHECK_FALSE(s.frame_path.has_value());
        CHECK_FALSE(s.video_path.has_value());
        CHECK(s.attempts == 0);
        CHECK(s.gate_notes.empty());
        CHECK_FALSE(s.duration_locked);
        CHECK_FALSE(s.needs_lipsync);
        CHECK(s.negative_prompt.empty());
        for (const models::DialogueLine& d : s.dialogue) {
            saw_dialogue = true;
            CHECK_FALSE(d.voice_id.has_value());
            CHECK_FALSE(d.audio_path.has_value());
            CHECK_FALSE(d.actual_duration_s.has_value());
        }
    }
    // 台词那三项要真的被测到，不然这一条只测了镜头级的那几个
    CHECK(saw_dialogue);
}

TEST_CASE("时长再平衡和 Python 一致") {
    const models::AssetLibrary a = test_assets();
    const std::string raw = golden().at("parses").at(0).at("raw").get<std::string>();
    for (const auto& c : golden().at("rebalances")) {
        const double target = c.at("target_s").get<double>();
        const double tol = c.at("tolerance_s").get<double>();
        CAPTURE(target);
        CAPTURE(tol);

        std::vector<models::Shot> shots = stages::parse_storyboard(raw, a);
        std::vector<double> before;
        for (const auto& s : shots) before.push_back(s.duration_s);
        CHECK(before == c.at("before").get<std::vector<double>>());

        stages::rebalance_durations(shots, target, tol);
        std::vector<double> after;
        for (const auto& s : shots) after.push_back(s.duration_s);
        CHECK(after == c.at("after").get<std::vector<double>>());
    }
}

TEST_CASE("再平衡只动没台词的镜头") {
    // 有台词的镜头时长由配音定，动了就对不上口型。
    const models::AssetLibrary a = test_assets();
    const std::string raw = golden().at("parses").at(0).at("raw").get<std::string>();
    std::vector<models::Shot> shots = stages::parse_storyboard(raw, a);

    std::vector<double> dialogue_before;
    for (const auto& s : shots) {
        if (!s.dialogue.empty()) dialogue_before.push_back(s.duration_s);
    }
    REQUIRE_FALSE(dialogue_before.empty());

    stages::rebalance_durations(shots, 5.0, 0.5);

    std::vector<double> dialogue_after;
    for (const auto& s : shots) {
        if (!s.dialogue.empty()) dialogue_after.push_back(s.duration_s);
    }
    CHECK(dialogue_before == dialogue_after);
}

// ---- 镜头数 ----
//
// 2026-09-12 加的。60 秒的集出过两镜六秒：配额那句话提示词里一个字没少，
// 但 shots 数组没有 minItems，两镜在语法上挑不出毛病。

TEST_CASE("分镜数的地板从目标时长和剧本的拍数推") {
    const auto q = stages::DurationQuota::for_duration(60.0);
    // 单镜最长 5 秒，60 秒至少 12 镜——这是物理下限，少于它总时长凑不够
    const auto b = stages::shot_count_bounds(q, 60.0, 24);
    CHECK(b.min_items == 12);
    CHECK(b.max_items == 48);
    // 剧本只有五行，硬要 12 镜出来的是空镜；地板退到拍数
    CHECK(stages::shot_count_bounds(q, 60.0, 5).min_items == 5);
    // 数不出拍数就按物理下限
    CHECK(stages::shot_count_bounds(q, 60.0, 0).min_items == 12);
}

TEST_CASE("schema 里带上镜头数的上下限，默认不带") {
    const models::AssetLibrary a = test_assets();
    const json with =
        json(stages::llm_shot_schema(a, stages::ShotCountBounds{12, 48}));
    CHECK(with.at("properties").at("shots").at("minItems") == 12);
    CHECK(with.at("properties").at("shots").at("maxItems") == 48);
    const json without = json(stages::llm_shot_schema(a));
    CHECK_FALSE(without.at("properties").at("shots").contains("minItems"));
}

TEST_CASE("数拍子时跳过段头和空行") {
    CHECK(stages::count_beats("【开场钩子 0–5 秒】\n天台，雨。\n\n林晚：你来了。\n"
                              "【冲突推进 5–33 秒】\n陈默：我不该来。") == 3);
    CHECK(stages::count_beats("") == 0);
}

// ---- 编号和覆盖 ----
//
// 2026-09-12 服务器实跑撞出来的两件事，都属于「模型写歪了但没人报」。

TEST_CASE("镜头编号和顺序由引擎重排") {
    std::vector<models::Shot> shots(4);
    // 实跑里模型给出来的那几种坏写法：错集号、没补零、打错字
    shots[0].shot_id = "ep61_sh002"; shots[0].order = 1;
    shots[1].shot_id = "ep01_sh001"; shots[1].order = 0;
    shots[2].shot_id = "ep01_s1h11"; shots[2].order = 3;
    shots[3].shot_id = "ep01_sh6";   shots[3].order = 3;  // order 重复

    stages::renumber_shots(shots, "ep01");

    // 按 order 排过之后编号是连号的，而且都带着对的集号
    std::vector<std::pair<std::string, int>> got;
    for (const auto& s : shots) got.push_back({s.shot_id, s.order});
    CHECK(got[1] == std::pair<std::string, int>{"ep01_sh001", 0});  // 原 order 0
    CHECK(got[0] == std::pair<std::string, int>{"ep01_sh002", 1});  // 原 order 1
    // order 重复的两个：stable，原来靠前的还是靠前
    CHECK(got[2] == std::pair<std::string, int>{"ep01_sh003", 2});
    CHECK(got[3] == std::pair<std::string, int>{"ep01_sh004", 3});

    std::set<std::string> ids;
    for (const auto& s : shots) ids.insert(s.shot_id);
    CHECK(ids.size() == shots.size());  // 不重名
}

TEST_CASE("剧本里的台词漏掉了要报出来") {
    const std::string script =
        "【开场钩子 0–5 秒】\n"
        "暴雨深夜，街头。\n"
        "林浩：这单要是超时，我这月房租就泡汤了。\n"
        "【集尾留扣 54–60 秒】\n"
        "苏婉：怎么了？脸色这么难看？\n"
        "林浩：你到底还瞒着我什么？";

    models::Shot s;
    s.shot_id = "ep01_sh001";
    s.scene_id = "sc01";
    models::CharacterInShot c;
    c.char_id = "c_lin_hao";
    s.characters.push_back(c);
    models::DialogueLine l;
    l.char_id = "c_lin_hao";
    l.text = "这单要是超时，我这月房租就泡汤了。";
    s.dialogue.push_back(l);

    // 只排了开场那一句，集尾留扣整段没进分镜——这一集丢的正是它的钩子。
    // **这不是致命错**：分镜表还能用，所以 check_coverage 不报，
    // 由 missing_dialogue_lines 单独列出来，交给界面去说。
    CHECK(stages::check_coverage(script, {s}).empty());
    const auto missing = stages::missing_dialogue_lines(script, {s});
    REQUIRE(missing.size() == 2);
    CHECK(missing[0] == "怎么了？脸色这么难看？");
    CHECK(missing[1] == "你到底还瞒着我什么？");

    SUBCASE("标点不一样不算丢") {
        // 模型把句号换成逗号、或者把一句拆成两镜，都是同一句话落地了
        models::Shot t = s;
        t.dialogue[0].text = "这单要是超时我这月房租就泡汤了";
        CHECK(stages::missing_dialogue_lines(
                  "林浩：这单要是超时，我这月房租就泡汤了。", {t}).empty());
    }

    SUBCASE("都排上了就不报") {
        models::Shot t = s;
        t.shot_id = "ep01_sh002";
        t.dialogue.clear();
        models::DialogueLine a, b;
        a.char_id = "c_su_wan";
        a.text = "怎么了？脸色这么难看？";
        b.char_id = "c_lin_hao";
        b.text = "你到底还瞒着我什么？";
        t.dialogue.push_back(a);
        t.dialogue.push_back(b);
        CHECK(stages::missing_dialogue_lines(script, {s, t}).empty());
        CHECK(stages::check_coverage(script, {s, t}).empty());
    }

    SUBCASE("一句都没写是致命的，那张表整个废了") {
        models::Shot mute = s;
        mute.dialogue.clear();
        const auto p = stages::check_coverage(script, {mute});
        REQUIRE(p.size() == 1);
        CHECK(p[0].find("一句台词都没有") != std::string::npos);
        // 这时候不该再把剧本里每一句都列一遍，同一个毛病说两遍
        CHECK(stages::missing_dialogue_lines(script, {mute}).empty());
    }

    SUBCASE("段头不算台词") {
        // 「【开场钩子 0–5 秒】」里没有冒号，但万一有别的带冒号的段头，
        // 也不该被当成一句要覆盖的台词
        CHECK(stages::missing_dialogue_lines("【情绪回报 33–54 秒】", {s}).empty());
    }
}

TEST_CASE("台词栏里的占位符要删掉，别让配音念出来") {
    // 实跑里模型写的那几种。十四镜里四镜是这样，而它会一路走到配音。
    CHECK(stages::is_placeholder_line("（无台词）"));
    CHECK(stages::is_placeholder_line("无台词"));
    CHECK(stages::is_placeholder_line("无台词。"));
    CHECK(stages::is_placeholder_line("(N/A)"));
    CHECK(stages::is_placeholder_line("None"));
    CHECK(stages::is_placeholder_line("无"));

    // 真台词一句都不能误删
    CHECK_FALSE(stages::is_placeholder_line("站住。"));
    CHECK_FALSE(stages::is_placeholder_line("为什么不能说？"));
    CHECK_FALSE(stages::is_placeholder_line("无所谓了。"));
    CHECK_FALSE(stages::is_placeholder_line("空房间里没有人。"));

    SUBCASE("解析时整句删掉，而且不会因为它多补一个在场角色") {
        const models::AssetLibrary a = test_assets();
        const json raw = {
            {"shots", json::array({
                {{"shot_id", "ep01_sh001"}, {"scene_id", "sc01"}, {"order", 0},
                 {"duration_s", 5.0}, {"shot_size", "MS"},
                 {"first_frame_prompt", "雨夜天台"},
                 {"characters", json::array()},
                 {"dialogue", json::array({
                     {{"char_id", "c_lin_wan"}, {"text", "（无台词）"}}})}},
                {{"shot_id", "ep01_sh002"}, {"scene_id", "sc01"}, {"order", 1},
                 {"duration_s", 3.0}, {"shot_size", "CU"},
                 {"first_frame_prompt", "近景"},
                 {"characters", json::array()},
                 {"dialogue", json::array({
                     {{"char_id", "c_lin_wan"}, {"text", "你说过会来的"}}})}}})}};
        const auto shots = stages::parse_storyboard(raw.dump(), a);
        REQUIRE(shots.size() == 2);
        CHECK(shots[0].dialogue.empty());
        // 占位那一句不该把角色补进来——这一镜画面里本来没人
        CHECK(shots[0].characters.empty());
        REQUIRE(shots[1].dialogue.size() == 1);
        CHECK(shots[1].dialogue[0].text == "你说过会来的");
        CHECK(shots[1].characters.size() == 1);
    }
}

TEST_CASE("越界的转场时长兜住，别为一栏装饰丢掉一整集") {
    // 实跑撞上的：模型给了 transition_dur_s = -0.4，validate 说「要在 0 到 2
    // 秒之间」，于是整张表连同另外十二个好镜头一起作废，84 秒的显卡时间没了。
    const models::AssetLibrary a = test_assets();
    auto one = [&](const char* trans, double dur) {
        const json raw = {
            {"shots", json::array({
                {{"shot_id", "ep01_sh001"}, {"scene_id", "sc01"}, {"order", 0},
                 {"duration_s", 5.0}, {"shot_size", "MS"},
                 {"first_frame_prompt", "雨夜"},
                 {"transition_in", trans}, {"transition_dur_s", dur},
                 {"characters", json::array({{{"char_id", "c_lin_wan"}}})},
                 {"dialogue", json::array({
                     {{"char_id", "c_lin_wan"}, {"text", "你说过会来的"}}})}}})}};
        const auto shots = stages::parse_storyboard(raw.dump(), a);
        REQUIRE(shots.size() == 1);
        return shots[0].transition_dur_s;
    };

    CHECK(one("dissolve", -0.4) == doctest::Approx(0.4));   // 负的
    CHECK(one("dissolve", 9.0) == doctest::Approx(0.4));    // 超过 2 秒
    CHECK(one("dissolve", 0.0) == doctest::Approx(0.4));    // 没填
    CHECK(one("dissolve", 0.8) == doctest::Approx(0.8));    // 合法的不动
    CHECK(one("cut", 1.5) == doctest::Approx(0.0));         // 硬切一律零
}

TEST_CASE("剧本里漏掉的台词由引擎照顺序补进镜头") {
    // 分镜模型不搬台词：实跑一集九句只写了两句，dump 出来看是压根没生成。
    // 台词本来就在剧本里，引擎自己放比指望模型重打一遍靠谱。
    const models::AssetLibrary a = test_assets();
    const std::string script =
        "【开场钩子 0–5 秒】\n"
        "雨夜天台。\n"
        "林晚：你说过会来的。\n"
        "他没有回头。\n"
        "陈默：我来了。\n"
        "【集尾留扣】\n"
        "林晚：晚了七年。";

    auto blank = [](const char* id, int order) {
        models::Shot s;
        s.shot_id = id;
        s.scene_id = "sc01";
        s.order = order;
        s.duration_s = 5.0;
        return s;
    };

    SUBCASE("一句都没写：整段按位置摊到各镜") {
        std::vector<models::Shot> shots{blank("ep01_sh001", 0),
                                        blank("ep01_sh002", 1),
                                        blank("ep01_sh003", 2)};
        CHECK(stages::place_missing_dialogue(shots, script, a) == 3);
        CHECK(stages::missing_dialogue_lines(script, shots).empty());
        // 顺序不能乱：先「你说过会来的」，最后「晚了七年」
        std::vector<std::string> said;
        for (const auto& s : shots)
            for (const auto& d : s.dialogue) said.push_back(d.text);
        CHECK(said == std::vector<std::string>{"你说过会来的。", "我来了。",
                                               "晚了七年。"});
        // 说话人认出来了，而且必然在场
        CHECK(shots[0].dialogue[0].char_id == std::optional<std::string>("c_lin_wan"));
        CHECK_FALSE(shots[0].characters.empty());
    }

    SUBCASE("写了一半：漏的那句插在两个锚点之间") {
        std::vector<models::Shot> shots{blank("ep01_sh001", 0),
                                        blank("ep01_sh002", 1),
                                        blank("ep01_sh003", 2)};
        models::DialogueLine first;
        first.char_id = "c_lin_wan";
        first.text = "你说过会来的。";
        shots[0].dialogue.push_back(first);
        models::DialogueLine last;
        last.char_id = "c_lin_wan";
        last.text = "晚了七年。";
        shots[2].dialogue.push_back(last);

        CHECK(stages::place_missing_dialogue(shots, script, a) == 1);
        // 只补中间那一句，而且落在首尾之间
        CHECK(shots[1].dialogue.size() == 1);
        CHECK(shots[1].dialogue[0].text == "我来了。");
        CHECK(shots[0].dialogue.size() == 1);
        CHECK(shots[2].dialogue.size() == 1);
    }

    SUBCASE("已经写全了就一句不补") {
        std::vector<models::Shot> shots{blank("ep01_sh001", 0)};
        for (const char* t : {"你说过会来的。", "我来了。", "晚了七年。"}) {
            models::DialogueLine d;
            d.char_id = "c_lin_wan";
            d.text = t;
            shots[0].dialogue.push_back(d);
        }
        CHECK(stages::place_missing_dialogue(shots, script, a) == 0);
        CHECK(shots[0].dialogue.size() == 3);
    }

    SUBCASE("没有镜头时什么也不做，不崩") {
        std::vector<models::Shot> none;
        CHECK(stages::place_missing_dialogue(none, script, a) == 0);
    }

    SUBCASE("不往一镜里堆到串音") {
        // 实跑撞上的：尾巴上几句没有后锚点，全挤进最后一镜——四句话配出来
        // 十几秒，而单镜上限五秒，后面的声音会盖到下一镜上。
        std::string many = "【开场钩子 0–5 秒】\n雨夜天台。\n";
        for (int i = 0; i < 8; ++i) {
            many += "林晚：这是第" + std::to_string(i) +
                    "句相当长的台词，长到一镜装不下两句。\n";
        }
        std::vector<models::Shot> shots;
        for (int i = 0; i < 8; ++i) shots.push_back(blank("x", i));
        // 先给最后一镜安一个锚点，逼出「后面没地方了」那种局面
        models::DialogueLine anchor;
        anchor.char_id = "c_lin_wan";
        anchor.text = "这是第0句相当长的台词，长到一镜装不下两句。";
        shots.back().dialogue.push_back(anchor);

        stages::place_missing_dialogue(shots, many, a);
        CHECK(stages::missing_dialogue_lines(many, shots).empty());

        const double cap = stages::max_line_seconds();
        for (const auto& s : shots) {
            double t = 0.0;
            for (const auto& d : s.dialogue) {
                t += stages::estimate_speech_duration(d.text);
            }
            CAPTURE(s.shot_id);
            CAPTURE(s.dialogue.size());
            // **单句本身就超上限是另一回事**：那种由配音那一步按标点拆开
            // （split_long_lines），落位管不了。这里要钉的是「不因为往一镜里
            // 堆了好几句而超」——四句挤在最后一镜那种。
            CHECK((s.dialogue.size() <= 1 || t <= cap));
        }
        // 八句没有全堆在一处
        std::size_t most = 0;
        for (const auto& s : shots) most = std::max(most, s.dialogue.size());
        CHECK(most <= 2);
    }
}

// ---- 视频模型的限制 ----
//
// 2026-09-13 从写死的常量改成配置项。写死的那个是 Wan 的（121 帧、4n+1），
// 而服务器 2026-09-09 就换成 MiniMax-H3 了（15 秒、17k+5）——换模型只改得了
// config.toml，改不到编译期常量。

TEST_CASE("视频限制：默认最保守，换一份就跟着变") {
    const stages::VideoLimits saved = stages::video_limits();

    SUBCASE("默认是最保守的那一档，什么都不知道时不放开") {
        // 没跑 Runtime::replace（单元测试）或者探测不到显卡时就是这一份。
        CHECK(stages::video_limits().max_frames == 121);
        CHECK(stages::max_shot_duration_s(24) == doctest::Approx(121.0 / 24.0));
        CHECK(stages::duration_slots() ==
              std::vector<double>{2.0, 3.0, 4.0, 5.0});
        CHECK(stages::frames_for(4.0, 24) == 97);
        CHECK(stages::frames_for(5.0, 24) == 121);
        CHECK(stages::frames_for(30.0, 24) == 121);
    }

    SUBCASE("换成 MiniMax-H3：档位放开，帧数落在 17k+5 上") {
        stages::VideoLimits h3;
        h3.max_frames = 360;
        h3.frame_step = 17;
        h3.frame_base = 5;
        stages::set_video_limits(h3);

        // 360 不在 17k+5 的格子上，往下取到 345 = 14.375 秒
        CHECK(stages::max_shot_duration_s(24) == doctest::Approx(345.0 / 24.0));
        // 一集不必再被切成十几个五秒片段
        CHECK(stages::duration_slots() ==
              std::vector<double>{2.0, 3.0, 4.0, 5.0, 6.0, 8.0, 10.0, 12.0});

        CHECK(stages::frames_for(4.0, 24) == 107);
        CHECK(stages::frames_for(3.0, 24) == 73);    // 72 → 73，正好在格子上
        CHECK((stages::frames_for(7.5, 24) - 5) % 17 == 0);

        // 真实时长跟着帧数走——装配要用它，差的那 0.458 秒逐镜累积
        CHECK(stages::video_limits().real_duration_s(4.0, 24) ==
              doctest::Approx(107.0 / 24.0));
        CHECK(stages::video_limits().real_duration_s(4.0, 24) > 4.0);

        // 配音的单句上限跟着放开：一句话不必再被切成一句一镜
        CHECK(stages::max_line_seconds(24) > 14.0);
    }

    SUBCASE("卡不够大时把模型的能力夹低") {
        stages::VideoLimits h3;
        h3.max_frames = 360;
        h3.frame_step = 17;
        h3.frame_base = 5;

        // 探测不到显卡：原样返回，不自作主张放开
        CHECK(stages::cap_by_vram(h3, 0.0, 0.0).max_frames == 360);

        // 5090 32.6 GB：算出来还是五秒那一档，和实测跑得动的一致
        const auto on5090 = stages::cap_by_vram(h3, 32.6, 0.0);
        CHECK(on5090.max_frames < 360);
        stages::set_video_limits(on5090);
        CHECK(stages::duration_slots() == std::vector<double>{2.0, 3.0, 4.0, 5.0});

        // 大卡才放开
        const auto big = stages::cap_by_vram(h3, 80.0, 0.0);
        CHECK(big.max_frames > on5090.max_frames);
        stages::set_video_limits(big);
        CHECK(stages::duration_slots().size() > 4);

        // 权重把卡占满：退回保守那一档，而不是夹到一两秒——跑不了五秒镜头
        // 的卡整条流水线本来也跑不动，排出一堆碎片没有意义。
        // **下限按秒定**，所以在 17k+5 这个格子上是 124 帧（5.167 秒），
        // 不是 121（那个向下取到 107 = 4.458 秒，反而不够五秒）。
        CHECK(stages::cap_by_vram(h3, 8.0, 8.0).max_frames == 124);
        // 小卡同理，至少保住五秒那一档
        stages::set_video_limits(stages::cap_by_vram(h3, 2.0, 0.0));
        CHECK(stages::duration_slots() == std::vector<double>{2.0, 3.0, 4.0, 5.0});
    }

    SUBCASE("上限小到一档都不剩时也有一档可用") {
        stages::VideoLimits tiny;
        tiny.max_frames = 12;   // 0.5 秒
        stages::set_video_limits(tiny);
        CHECK_FALSE(stages::duration_slots().empty());
        // 12 不在 4n+1 的格子上，往下取到 9
        CHECK(stages::frames_for(10.0, 24) == 9);
    }

    stages::set_video_limits(saved);
}

TEST_CASE("视频限制按模型自己认，不用人记着填") {
    // **换模型时人改的是 [models].video 那一行**，不会想起来还有帧数格子
    // 要跟着改；而填错了全程不报错，只是成片比分镜表长一点点。
    const auto h3 = stages::guess_video_limits("minimax_h3_fl2va-Q4_K_M.gguf");
    CHECK(h3.max_frames == 360);
    CHECK(h3.frame_step == 17);
    CHECK(h3.frame_base == 5);

    const auto wan = stages::guess_video_limits("wan2.1_i2v_480p_14B_fp16.safetensors");
    CHECK(wan.max_frames == 121);
    CHECK(wan.frame_step == 4);
    CHECK(wan.frame_base == 1);

    // 大小写和别名
    CHECK(stages::guess_video_limits("MiniMax_H3.gguf").frame_step == 17);
    CHECK(stages::guess_video_limits("hailuo3_q4.gguf").frame_step == 17);

    SUBCASE("名字认不出就看编码器走哪条路") {
        // H3 挂 video_llm（Qwen3-VL），Wan 挂 video_text_encoder（UMT5-XXL），
        // 配置里这两项只能填一个。
        CHECK(stages::guess_video_limits("my_model.gguf", true).frame_step == 17);
        CHECK(stages::guess_video_limits("my_model.gguf", false).frame_step == 4);
    }

    SUBCASE("两处都认不出退回最保守的一档") {
        // 宁可把镜头限短：按短的排最坏是浪费了本事，片子照出；按长的排而
        // 模型其实出不了，得到的是被静默截断的片子。
        const auto unknown = stages::guess_video_limits("");
        CHECK(unknown.max_frames == 121);
        CHECK(unknown.frame_step == 4);
    }
}

TEST_CASE("帧率也是模型说了算：H3 只出 24fps") {
    // 上游 docs/minimax_h3.md：「MiniMax-H3 runs at 24 fps; another requested
    // value is overridden」。sd.cpp 里是硬改 + 一句 LOG_WARN，而那句警告
    // 淹在一屏 CUDA Graph 日志里。
    //
    // **不跟着改的后果是全片变速，不是报错**：裸帧按 24fps 的节奏演出来，
    // 我们却按 [assembly].fps 去编码和算时长。填 30 就是每一镜快 25%。
    const auto h3 = stages::guess_video_limits("minimax_h3_fl2va-Q4_K_M.gguf");
    CHECK(h3.native_fps == 24);
    CHECK(stages::effective_fps(h3, 30) == 24);
    CHECK(stages::effective_fps(h3, 16) == 24);
    CHECK(stages::effective_fps(h3, 24) == 24);

    SUBCASE("Wan 不挑：传什么用什么") {
        // sd.cpp 对 Wan 不做覆盖，所以这儿也不该替人做主。
        const auto wan = stages::guess_video_limits("wan2.2_ti2v_5B.gguf");
        CHECK(wan.native_fps == 0);
        CHECK(stages::effective_fps(wan, 30) == 30);
        CHECK(stages::effective_fps(wan, 16) == 16);
    }

    SUBCASE("配置里填了 0 或负数，退回 24") {
        const auto wan = stages::guess_video_limits("wan2.2_ti2v_5B.gguf");
        CHECK(stages::effective_fps(wan, 0) == 24);
        CHECK(stages::effective_fps(wan, -5) == 24);
    }
}

// ---- 名义秒 vs 成片秒 ----

TEST_CASE("再平衡压的是成片长度，不是分镜表上那串名义值") {
    // **同一集两套秒。** 分镜表里的 duration_s 是从档位表 {2,3,4,5,…} 里挑
    // 的整数，而模型只能按格子出帧（H3 是 17k+5），名义 5 秒出来是 124 帧
    // = 5.167 秒。装配那边一直按真值排时间轴（media/assemble.cpp），规划
    // 这边却一直按名义值加——rebalance 精算到"60 秒"，片子是 62 秒。
    //
    // Wan 那会儿每镜只差 0.042 秒（1%），藏得住；换 H3 之后单镜最多差
    // 0.583 秒，就露出来了。**测试必须用 H3 的格子，用默认那份测不出来。**
    const stages::VideoLimits saved = stages::video_limits();

    stages::VideoLimits h3;
    h3.max_frames = 124;   // 五秒那一档，档位表 = {2,3,4,5}
    h3.frame_step = 17;
    h3.frame_base = 5;
    stages::set_video_limits(h3);
    REQUIRE(stages::duration_slots() == std::vector<double>{2.0, 3.0, 4.0, 5.0});

    const auto make = [](const std::string& id, double dur, bool talks) {
        models::Shot s;
        s.shot_id = id;
        s.duration_s = dur;
        if (talks) {
            models::DialogueLine line;
            line.text = "台词";
            s.dialogue.push_back(line);
        }
        return s;
    };

    // 4 个台词镜（配音锁死、动不了）+ 8 个过渡镜，全是名义 5 秒。
    std::vector<models::Shot> shots;
    for (int i = 0; i < 4; ++i) {
        shots.push_back(make("talk" + std::to_string(i), 5.0, true));
    }
    for (int i = 0; i < 8; ++i) {
        shots.push_back(make("gap" + std::to_string(i), 5.0, false));
    }

    // 名义上正好 60 秒——按名义值记账的话这一集"已经达标了"。
    double nominal = 0.0;
    for (const auto& s : shots) nominal += s.duration_s;
    REQUIRE(nominal == doctest::Approx(60.0));
    // 而它真正会出 12 × 5.167 = 62.0 秒。**这 2 秒就是旧版看不见的那部分。**
    REQUIRE(stages::real_total_s(shots, 24) == doctest::Approx(12 * 124.0 / 24.0));
    REQUIRE(stages::real_total_s(shots, 24) > 61.9);

    stages::rebalance_durations(shots, 60.0, 0.5, 24);

    // 修好之后：成片长度回到 60 秒附近。
    CHECK(stages::real_total_s(shots, 24) == doctest::Approx(60.0).epsilon(0.01));

    // 而名义总长**不再是 60**——正是这一条把两种记账方式区分开。
    // 旧版（按名义值算）在这里会一动不动地返回，名义和成片都停在原处。
    double after_nominal = 0.0;
    for (const auto& s : shots) after_nominal += s.duration_s;
    CHECK(after_nominal < 59.0);

    // 台词镜一帧没动：它们的时长是配音定的，动了就对不上口型。
    for (const auto& s : shots) {
        if (!s.dialogue.empty()) CHECK(s.duration_s == doctest::Approx(5.0));
    }

    stages::set_video_limits(saved);
}

TEST_CASE("real_total_s 数的是帧，不是分镜表") {
    const stages::VideoLimits saved = stages::video_limits();

    stages::VideoLimits h3;
    h3.max_frames = 360;
    h3.frame_step = 17;
    h3.frame_base = 5;
    stages::set_video_limits(h3);

    models::Shot a;
    a.duration_s = 4.0;   // 96 帧 → 向上对齐到 107 = 4.458 秒
    models::Shot b;
    b.duration_s = 2.0;   // 48 帧 → 56 = 2.333 秒
    const std::vector<models::Shot> shots{a, b};

    CHECK(stages::real_total_s(shots, 24) ==
          doctest::Approx((107.0 + 56.0) / 24.0));
    // 名义 6 秒，实际 6.79 秒——每一镜都只多一点，十几镜就是好几秒。
    CHECK(stages::real_total_s(shots, 24) > 6.7);

    SUBCASE("空的一集是 0，不是 NaN") {
        CHECK(stages::real_total_s({}, 24) == doctest::Approx(0.0));
    }

    stages::set_video_limits(saved);
}

TEST_CASE("按画布夹内核跑得动的帧数上限：不是显存，是序列长度") {
    // 2026-09-13 5090 实测（704×1280）：192 帧跑得动，294 帧整个引擎
    // 当场死（CUDA invalid configuration argument）。数在 limits.hpp。
    stages::VideoLimits h3;
    h3.max_frames = 360;
    h3.frame_step = 17;
    h3.frame_base = 5;

    // 704×1280：上限落在实测跑得动的 192，横竖一样
    CHECK(stages::cap_by_kernel_limit(h3, 704, 1280).max_frames == 192);
    CHECK(stages::cap_by_kernel_limit(h3, 1280, 704).max_frames == 192);

    // 544×928：换算出来 342，比模型自己的 360 低一点，但仍远高于 5090 上
    // 显存那一道——所以在这张卡、这个画幅上什么都不变
    const auto std_canvas = stages::cap_by_kernel_limit(h3, 544, 928);
    CHECK(std_canvas.max_frames == 342);
    CHECK(std_canvas.max_frames > stages::cap_by_vram(h3, 32.6, 0.0).max_frames);

    // 大卡上 cap_by_vram 放开到十几秒，这一道要把它按回来
    const auto big = stages::cap_by_kernel_limit(
        stages::cap_by_vram(h3, 80.0, 0.0), 704, 1280);
    CHECK(big.max_frames == 192);

    // 2K：只剩 46 帧，**没有五秒地板**——地板抬上去就是让分镜排出必炸的镜头
    CHECK(stages::cap_by_kernel_limit(h3, 1440, 2560).max_frames == 46);

    // 不知道画布就不动；已经比天花板低的不会被抬高
    CHECK(stages::cap_by_kernel_limit(h3, 0, 0).max_frames == 360);
    stages::VideoLimits wan;
    wan.max_frames = 121;
    CHECK(stages::cap_by_kernel_limit(wan, 704, 1280).max_frames == 121);
}
