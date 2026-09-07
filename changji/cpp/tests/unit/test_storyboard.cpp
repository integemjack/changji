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

stages::DurationQuota quota_from(const json& slots) {
    stages::DurationQuota q;
    for (const auto& pair : slots) {
        q.slots[pair[0].get<double>()] = pair[1].get<int>();
    }
    return q;
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
