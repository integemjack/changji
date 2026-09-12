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
