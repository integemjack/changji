// 大纲生成与故事接口。
//
// 提示词这一份是新写的、没有 Python 对应物，所以**不做逐字节对拍**——
// 那样只会把提示词冻死。这里钉的是那几条不能松的规矩：
//
//   · schema 里不许出现任何外观字段（给了模型就会写一遍长相，
//     而那份长相和后面美术那一步出的必然对不上）
//   · 章节 id 由程序生成，不听模型的
//   · 关系的两端必须是登记过的人，指不到的直接丢
//   · /api/story/outline **只回草稿不落库**
//
// 大模型一律用 ReplayClient 桩。这台机器上不验 LLM 质量。

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "http/batch.hpp"
#include "http/planning.hpp"
#include "http/story_api.hpp"
#include "llm/client.hpp"
#include "models/project.hpp"
#include "models/story.hpp"
#include "stages/bible.hpp"
#include "stages/script_story.hpp"
#include "stages/chapter_write.hpp"
#include "stages/story_analyze.hpp"
#include "stages/story_import.hpp"
#include "stages/story_outline.hpp"
#include "stages/story_plan.hpp"
#include "util/paths.hpp"
#include "util/text.hpp"

using namespace changji;
using namespace changji::models;
using changji::stages::build_outline_prompt;
using changji::stages::outline_schema;
using changji::stages::parse_outline;

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

/// 一份像样的模型返回。各用例在它上面改。
json good_outline() {
    return json{
        {"logline", "一把伞牵出五年前的事"},
        {"genre", "都市情感"},
        {"tone", "克制"},
        {"characters",
         json::array({
             json{{"name", "林晚"},
                  {"identity", "便利店夜班店员"},
                  {"want", "把那把伞还回去然后彻底了断"},
                  {"arc", "从躲着走到敢直视"}},
             json{{"name", "陈默"},
                  {"identity", "回城出差的建筑师"},
                  {"want", "问出当年她为什么不告而别"},
                  {"arc", "从质问到放下"}},
         })},
        {"relations",
         json::array({
             json{{"a", "林晚"},
                  {"b", "陈默"},
                  {"kind", "前任"},
                  {"tension", "谁都欠对方一句没说出口的道歉"}},
         })},
        {"locations",
         json::array({
             json{{"name", "便利店"},
                  {"what", "高架桥下那家 24 小时便利店"},
                  {"when", "深夜，冷白顶光"}},
         })},
        {"chapters",
         json::array({
             json{{"title", "雨夜重逢"},
                  {"summary", "他推门进来，伞还在手里。"},
                  {"hook", "她认出那把伞"},
                  {"characters", json::array({"林晚", "陈默"})},
                  {"locations", json::array({"便利店"})}},
             json{{"title", "五年前那把伞"},
                  {"summary", "回到五年前的那个雨夜。"},
                  {"hook", "他没有回头"},
                  {"characters", json::array({"林晚"})},
                  {"locations", json::array({"便利店"})}},
         })},
    };
}

fs::path fresh_project(const std::string& tag) {
    const fs::path root =
        fs::temp_directory_path() / paths::from_utf8("changji_故事接口_" + tag);
    std::error_code ec;
    fs::remove_all(root, ec);
    ProjectStore::create(root, "gushi", "故事接口测试");
    return root;
}

std::string p_str(const fs::path& p) { return paths::to_utf8(p); }

}  // namespace

TEST_CASE("schema 里不许有外观字段") {
    const auto& props = outline_schema().at("properties");
    const auto& ch = props.at("characters").at("items").at("properties");

    // 这几个是资产库那边的字段。出现在这里就意味着模型会在故事层写一遍长相，
    // 而 bible 那一步还会再写一遍，两份必然不一致——那正是要防的漂移。
    for (const char* banned : {"face", "body", "attire", "appearance", "look"}) {
        CHECK_MESSAGE(!ch.contains(banned),
                      "大纲 schema 里冒出了外观字段 " << banned);
    }
    CHECK(ch.contains("name"));
    CHECK(ch.contains("want"));

    // 章节必须有 hook：后面每一集的结尾都要落在钩子上，没有钩子就只能按
    // 字数硬切，那是这套设计要避开的东西。
    const auto& chap = props.at("chapters").at("items");
    CHECK(chap.at("properties").contains("hook"));
    bool hook_required = false;
    for (const auto& r : chap.at("required")) {
        if (r == "hook") hook_required = true;
    }
    CHECK(hook_required);
}

TEST_CASE("地点和出场人要写进 required，不然 14B 一个都不给") {
    // **这一条钉的是一次实跑。** 2026-09-11 从零走一遍：出大纲给了 3 个
    // 人物、3 条关系（都在 required 里），而 `locations` 是空数组，每一章
    // 的 `characters` 和 `locations` 也全是空的——那几项当时都不在 required
    // 里。读一遍正文（analyze）出来的一模一样。不是模型读不懂，是语法采样
    // 允许它们缺席，它就缺席。
    //
    // 缺了不是"少一点信息"：设定页的「场景」那一格永远是空的，空景图无从
    // 谈起；再往后排分镜时，不知道这一章在哪儿发生、谁在场——而那正是
    // 第二步全部的输入。
    const auto top_level = [](const nlohmann::ordered_json& schema,
                              const char* who) {
        CAPTURE(who);
        bool top_locations = false;
        bool top_relations = false;
        for (const auto& r : schema.at("required")) {
            if (r == "locations") top_locations = true;
            if (r == "relations") top_relations = true;
        }
        CHECK_MESSAGE(top_locations, who << " 的 locations 不在 required 里");
        // relations 同理，而且它更险：**采用那一步会拿空数组盖掉已经有的
        // 那几条**。实跑里出大纲给了 3 条，读完正文变成 0 条。
        CHECK_MESSAGE(top_relations, who << " 的 relations 不在 required 里");
        // 空数组也是合法的数组，所以光 required 不够，还要有下限
        CHECK(schema.at("properties").at("locations").at("minItems") == 1);
    };
    top_level(outline_schema(), "大纲");
    top_level(changji::stages::analyze_schema(), "读故事");

    // 每章那两份名单**只要求「读故事」那一份**。
    //
    // 大纲那边加过，加完出一份大纲从三十几秒变成 278 秒，还截断在半截
    // JSON 上——语法一收紧，14B 就一路写到 token 上限也收不了口。而这两项
    // 本来就是照着正文读出来的准，大纲阶段凭空想的不准。
    const auto& chap =
        changji::stages::analyze_schema().at("properties").at("chapters").at("items");
    bool ch_chars = false;
    bool ch_locs = false;
    for (const auto& r : chap.at("required")) {
        if (r == "characters") ch_chars = true;
        if (r == "locations") ch_locs = true;
    }
    CHECK_MESSAGE(ch_chars, "读故事的每章 characters 不在 required 里");
    CHECK_MESSAGE(ch_locs, "读故事的每章 locations 不在 required 里");
    CHECK(chap.at("properties").at("characters").at("minItems") == 1);
    CHECK(chap.at("properties").at("locations").at("minItems") == 1);
}

TEST_CASE("提示词：章数跟着体量走，不跟集数走") {
    const std::string s = build_outline_prompt("深夜便利店", StoryScale::SHORT,
                                               StyleLine::REALISTIC);
    const std::string m = build_outline_prompt("深夜便利店", StoryScale::MEDIUM,
                                               StyleLine::REALISTIC);
    const std::string l = build_outline_prompt("深夜便利店", StoryScale::LONG,
                                               StyleLine::REALISTIC);

    CHECK(s.find("写成 4 章左右") != std::string::npos);
    CHECK(m.find("写成 8 章左右") != std::string::npos);
    CHECK(l.find("写成 16 章左右") != std::string::npos);

    // 梗概进得去
    CHECK(m.find("深夜便利店") != std::string::npos);
    // 「不要写长相」这条必须在提示词里，schema 挡得住字段挡不住它写进 summary
    CHECK(m.find("不要写任何长相") != std::string::npos);
    // 完整故事要有结尾，这是和「无限续写」的分界
    CHECK(m.find("有结尾的完整故事") != std::string::npos);
    // 实跑时四章写的是同一个场面（便利店、门铃、她拿着伞进来）换三个角度，
    // 根子在大纲：每章必须把故事往前挪
    CHECK(m.find("每一章都要把故事往前挪一步") != std::string::npos);
}

TEST_CASE("提示词：画风和关键词") {
    const std::string anime =
        build_outline_prompt("梗概", StoryScale::MEDIUM, StyleLine::ANIME);
    CHECK(anime.find("动漫短剧") != std::string::npos);

    const std::string real =
        build_outline_prompt("梗概", StoryScale::MEDIUM, StyleLine::REALISTIC);
    CHECK(real.find("真人写实短剧") != std::string::npos);

    const std::string kw = build_outline_prompt("梗概", StoryScale::MEDIUM,
                                                StyleLine::REALISTIC, "重生复仇");
    CHECK(kw.find("往这个方向想：重生复仇") != std::string::npos);
    // 没给关键词时那一段整块不出现
    CHECK(real.find("往这个方向想") == std::string::npos);
}

TEST_CASE("解析大纲") {
    const Story s =
        parse_outline(good_outline().dump(), "深夜便利店", StoryScale::MEDIUM);

    CHECK(s.premise == "深夜便利店");
    CHECK(s.scale == StoryScale::MEDIUM);
    CHECK(s.source == StorySource::AI);
    CHECK(s.logline == "一把伞牵出五年前的事");
    REQUIRE(s.characters.size() == 2);
    CHECK(s.characters[0].name == "林晚");
    REQUIRE(s.relations.size() == 1);
    CHECK(s.relations[0].tension == "谁都欠对方一句没说出口的道歉");

    REQUIRE(s.chapters.size() == 2);
    // 章节 id 是程序生成的，模型说了不算
    CHECK(s.chapters[0].chapter_id == "ch01");
    CHECK(s.chapters[1].chapter_id == "ch02");
    CHECK(s.chapters[0].title == "雨夜重逢");
    // 大纲阶段没有正文，钩子挂在 0 上——正文为空时 0 既是章首也是章尾
    REQUIRE(s.chapters[0].hooks.size() == 1);
    CHECK(s.chapters[0].hooks[0].at_char == 0);
    CHECK(s.chapters[0].hooks[0].text == "她认出那把伞");
    CHECK(s.chapters[0].characters.size() == 2);

    // 自己产出的东西要能过自己的校验
    CHECK(s.validate().empty());
}

TEST_CASE("解析：把会坏事的东西挡在外面") {
    SUBCASE("关系指向没登记的人——丢掉，不能留") {
        json j = good_outline();
        j["relations"].push_back(json{{"a", "林晚"},
                                      {"b", "查无此人"},
                                      {"kind", "邻居"},
                                      {"tension", "无"}});
        const Story s = parse_outline(j.dump(), "梗概", StoryScale::MEDIUM);
        CHECK(s.relations.size() == 1);
        CHECK(s.validate().empty());
    }

    SUBCASE("人物重名——只留第一个") {
        json j = good_outline();
        j["characters"].push_back(
            json{{"name", "林晚"}, {"identity", "另一个林晚"}, {"want", "x"}});
        const Story s = parse_outline(j.dump(), "梗概", StoryScale::MEDIUM);
        CHECK(s.characters.size() == 2);
        CHECK(s.characters[0].identity == "便利店夜班店员");
    }

    SUBCASE("没名字的人物——丢掉，否则成片里会出现一个叫空串的角色") {
        json j = good_outline();
        j["characters"].push_back(json{{"name", "  "}, {"identity", "路人"}});
        const Story s = parse_outline(j.dump(), "梗概", StoryScale::MEDIUM);
        CHECK(s.characters.size() == 2);
    }

    SUBCASE("章节里冒出没登记的人——过滤掉") {
        json j = good_outline();
        j["chapters"][0]["characters"].push_back("查无此人");
        const Story s = parse_outline(j.dump(), "梗概", StoryScale::MEDIUM);
        CHECK(s.chapters[0].characters.size() == 2);
    }

    SUBCASE("一章都没有——报错，不要静悄悄产出一个空故事") {
        json j = good_outline();
        j["chapters"] = json::array();
        CHECK_THROWS_AS(parse_outline(j.dump(), "梗概", StoryScale::MEDIUM),
                        stages::StoryError);
    }

    SUBCASE("根本不是 JSON") {
        CHECK_THROWS_AS(parse_outline("模型今天想聊点别的", "梗概",
                                      StoryScale::MEDIUM),
                        stages::StoryError);
    }

    SUBCASE("裹着 ```json 外壳也要能读出来") {
        const std::string raw = "```json\n" + good_outline().dump() + "\n```";
        const Story s = parse_outline(raw, "梗概", StoryScale::MEDIUM);
        CHECK(s.chapters.size() == 2);
    }
}

TEST_CASE("GET /api/story：没有 story.json 时回空故事，不是 404") {
    const fs::path root = fresh_project("空");
    const auto r = http::get_story(p_str(root));
    CHECK(r.status == 200);
    CHECK(r.body.at("empty").get<bool>());
    CHECK(r.body.at("chapters").get<int>() == 0);
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story：梗概同时写回 project.json") {
    const fs::path root = fresh_project("梗概");
    const auto r = http::post_story(json{{"project", p_str(root)},
                                         {"premise", "  深夜便利店，前任推门进来。  "},
                                         {"scale", "long"},
                                         {"episode_duration_s", 90.0}});
    CHECK(r.status == 200);

    ProjectStore store(root);
    const Story s = store.load_story();
    CHECK(s.premise == "深夜便利店，前任推门进来。");
    CHECK(s.scale == StoryScale::LONG);
    CHECK(s.episode_duration_s == doctest::Approx(90.0));

    // 老流程的写剧本提示词读的是 Project::premise，两边必须是同一句
    CHECK(store.load_project().premise == "深夜便利店，前任推门进来。");

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story：体量拼错了要报错，不能悄悄当成短篇") {
    const fs::path root = fresh_project("体量");
    CHECK_THROWS_AS(
        http::post_story(json{{"project", p_str(root)}, {"scale", "midium"}}),
        http::ApiError);
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/outline：只回草稿，不落库") {
    const fs::path root = fresh_project("草稿");
    llm::ReplayClient client({good_outline().dump()});
    pipeline::CancelToken tok;

    const auto r = http::post_story_outline(
        json{{"project", p_str(root)}, {"premise", "深夜便利店"}}, client, tok);

    CHECK(r.status == 200);
    CHECK_FALSE(r.body.at("adopted").get<bool>());
    CHECK(r.body.at("chapters").get<int>() == 2);
    // 大纲阶段没正文，一章一集
    CHECK(r.body.at("episodes").get<int>() == 2);

    // **盘上还是空的。** 源头没人审过就落库，后面几十分钟渲染全是白跑。
    ProjectStore store(root);
    CHECK(store.load_story().empty());

    // 提示词确实拼过并发出去了
    REQUIRE(client.calls().size() == 1);
    CHECK(client.calls()[0].prompt.find("深夜便利店") != std::string::npos);
    CHECK(client.calls()[0].schema_name == "story_outline");

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/adopt：落库并算出分集表") {
    const fs::path root = fresh_project("采用");
    const Story draft =
        parse_outline(good_outline().dump(), "深夜便利店", StoryScale::MEDIUM);

    const auto r = http::post_story_adopt(
        json{{"project", p_str(root)}, {"story", json(draft)}});
    CHECK(r.status == 200);
    CHECK(r.body.at("adopted").get<bool>());

    ProjectStore store(root);
    const Story saved = store.load_story();
    CHECK(saved.chapters.size() == 2);
    CHECK(saved.plan.size() == 2);
    CHECK(saved.plan[0].episode_id == "ep01");
    CHECK(saved.plan[0].hook == "她认出那把伞");
    // 梗概照样同步回 project.json
    CHECK(store.load_project().premise == "深夜便利店");

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/adopt：会顶掉写好的正文时要拦一下") {
    const fs::path root = fresh_project("顶掉");
    ProjectStore store(root);

    // 先存一份已经展开过正文的故事
    Story old;
    old.premise = "老故事";
    Chapter c;
    c.chapter_id = "ch01";
    c.title = "写过的一章";
    c.text = "这一章已经有正文了。";
    old.chapters.push_back(c);
    store.save_story(old);

    const Story draft =
        parse_outline(good_outline().dump(), "新梗概", StoryScale::MEDIUM);

    // 不带 overwrite：409，正文还在
    CHECK_THROWS_AS(http::post_story_adopt(
                        json{{"project", p_str(root)}, {"story", json(draft)}}),
                    http::ApiError);
    CHECK(store.load_story().written_chapters() == 1);

    // 带上 overwrite 才换
    const auto r = http::post_story_adopt(json{{"project", p_str(root)},
                                               {"story", json(draft)},
                                               {"overwrite", true}});
    CHECK(r.status == 200);
    CHECK(store.load_story().chapters.size() == 2);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/plan：改每集时长，集数跟着变") {
    const fs::path root = fresh_project("重算");
    ProjectStore store(root);

    Story s;
    s.premise = "梗概";
    Chapter c;
    c.chapter_id = "ch01";
    c.title = "雨夜重逢";
    for (int i = 0; i < 3000; ++i) c.text += "字";
    for (int at = 300; at < 3000; at += 300) {
        Hook h;
        h.at_char = at;
        h.text = "钩子";
        c.hooks.push_back(h);
    }
    s.chapters.push_back(c);
    store.save_story(s);

    const auto few = http::post_story_plan(
        json{{"project", p_str(root)}, {"duration_s", 120.0}});
    const int few_n = few.body.at("episodes").get<int>();

    const auto many = http::post_story_plan(
        json{{"project", p_str(root)}, {"duration_s", 30.0}});
    const int many_n = many.body.at("episodes").get<int>();

    CHECK(few_n < many_n);
    // 重算的结果要落盘，不是只回给前端
    CHECK(store.load_story().plan.size() == static_cast<std::size_t>(many_n));
    CHECK(store.load_story().episode_duration_s == doctest::Approx(30.0));

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/plan：还没有故事就说清楚") {
    const fs::path root = fresh_project("没故事");
    CHECK_THROWS_AS(http::post_story_plan(json{{"project", p_str(root)}}),
                    http::ApiError);
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("多余字段一律 422") {
    const fs::path root = fresh_project("多余");
    try {
        http::post_story(json{{"project", p_str(root)}, {"episodes", 3}});
        FAIL("应该抛");
    } catch (const http::ApiError& e) {
        // 「我要写 N 集」这个入参没有了，传上来要被顶回去
        CHECK(e.status() == 422);
    }
    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---- 期 3：圣经的名单从故事来，不再从第一集剧本里找 ----

namespace {

/// 美术那一步的模型返回。
json good_bible() {
    return json{
        {"characters",
         json::array({
             json{{"key", "lin_wan"},
                  {"name", "林晚"},
                  {"identity", "二十七八岁女性，克制"},
                  {"body", "偏瘦，肩背挺"},
                  {"face", "齐肩黑直发，圆眼，单眼皮"},
                  {"attire", "便利店藏青制服外套"}},
             json{{"key", "chen_mo"},
                  {"name", "陈默"},
                  {"identity", "三十出头男性，沉静"},
                  {"body", "中等身量"},
                  {"face", "短寸黑发，方脸，浓眉"},
                  {"attire", "深灰风衣"}},
         })},
        {"locations",
         json::array({
             json{{"key", "store_night"},
                  {"name", "便利店"},
                  {"space", "临街玻璃门，两排货架"},
                  {"lighting", "夜间冷白顶光，玻璃上有雨痕"},
                  {"palette", "冷青加一点暖黄"}},
         })},
        {"global_style", "夜戏，低饱和，轻微颗粒"},
    };
}

Story sample_story() {
    return parse_outline(good_outline().dump(), "深夜便利店", StoryScale::MEDIUM);
}

}  // namespace

TEST_CASE("渲染给美术看的那一段：名单一个都不能少") {
    const std::string t = stages::render_story_for_bible(sample_story());

    // 名单是这一段的全部意义。漏一个人，后面分镜里就指不到它。
    CHECK(t.find("林晚") != std::string::npos);
    CHECK(t.find("陈默") != std::string::npos);
    CHECK(t.find("便利店") != std::string::npos);

    // 调子要在名单前面——放后面的话模型把人都写完了才读到"克制"
    CHECK(t.find("克制") < t.find("林晚"));

    // 关系给进去了。这是老路径完全没有的东西
    CHECK(t.find("前任") != std::string::npos);
    CHECK(t.find("谁都欠对方一句没说出口的道歉") != std::string::npos);

    // 欲望进去是让美术判断气质用的
    CHECK(t.find("他要的是") != std::string::npos);

    // 地点的时间和光要带上，lighting 要照着它写
    CHECK(t.find("深夜，冷白顶光") != std::string::npos);

    // 分章只当调子参考
    CHECK(t.find("雨夜重逢") != std::string::npos);
}

TEST_CASE("从故事出的圣经提示词：名单给定，只定妆") {
    const Story story = sample_story();
    const std::string p =
        stages::build_bible_prompt_from_story(story, StyleLine::REALISTIC);

    // 这条是新老两条路的分界：老的是"找出角色"，新的是"给这份名单定妆"
    CHECK(p.find("一个不许多，一个不许少") != std::string::npos);
    // 名字必须照抄，后面每一镜按名字找角色
    CHECK(p.find("逐字一样") != std::string::npos);
    // 剧作信息不许写进外观
    CHECK(p.find("不要把它们写进外观") != std::string::npos);
    // 画风和 face 那条老规矩要留着
    CHECK(p.find("真人写实") != std::string::npos);
    CHECK(p.find("face") != std::string::npos);
    // 故事那一段确实拼进去了
    CHECK(p.find(stages::render_story_for_bible(story)) != std::string::npos);

    const std::string anime =
        stages::build_bible_prompt_from_story(story, StyleLine::ANIME);
    CHECK(anime.find("二次元动漫") != std::string::npos);
}

TEST_CASE("POST /api/bible：项目里有故事就从故事出") {
    const fs::path root = fresh_project("圣经故事");
    ProjectStore store(root);
    Story story = sample_story();
    store.save_story(story);

    llm::ReplayClient client({good_bible().dump()});
    pipeline::CancelToken tok;
    const auto r = http::post_bible(json{{"project", p_str(root)}}, client, tok);

    CHECK(r.status == 200);
    // 回包要说清这次名单是从哪来的——「为什么这次多出来三个人」靠它解释
    CHECK(r.body.at("source").get<std::string>() == "story");
    CHECK(r.body.at("added_characters").size() == 2);

    // 发出去的提示词走的是故事那条
    REQUIRE(client.calls().size() == 1);
    CHECK(client.calls()[0].prompt.find("一个不许多，一个不许少") !=
          std::string::npos);
    // 而且**没有**去读任何一集剧本
    CHECK(client.calls()[0].prompt.find("读下面的剧本") == std::string::npos);

    CHECK(store.load_assets().characters.size() == 2);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/bible：没有故事的老项目照旧走剧本那条") {
    const fs::path root = fresh_project("圣经剧本");
    ProjectStore store(root);
    Project project = store.load_project();
    Episode ep;
    ep.episode_id = "ep01";
    ep.title = "雨夜重逢";
    ep.script = "林晚：你还留着它。\n陈默把伞放在柜台上。";
    project.episodes.push_back(ep);
    store.save_project(project);

    llm::ReplayClient client({good_bible().dump()});
    pipeline::CancelToken tok;
    const auto r = http::post_bible(json{{"project", p_str(root)}}, client, tok);

    CHECK(r.status == 200);
    CHECK(r.body.at("source").get<std::string>() == "script");
    REQUIRE(client.calls().size() == 1);
    CHECK(client.calls()[0].prompt.find("读下面的剧本") != std::string::npos);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/bible：source 能强制走哪条") {
    const fs::path root = fresh_project("圣经强制");
    ProjectStore store(root);
    store.save_story(sample_story());
    Project project = store.load_project();
    Episode ep;
    ep.episode_id = "ep01";
    ep.script = "林晚：你还留着它。";
    project.episodes.push_back(ep);
    store.save_project(project);

    SUBCASE("有故事也能按剧本出") {
        llm::ReplayClient client({good_bible().dump()});
        pipeline::CancelToken tok;
        const auto r = http::post_bible(
            json{{"project", p_str(root)}, {"source", "script"}}, client, tok);
        CHECK(r.body.at("source").get<std::string>() == "script");
    }

    SUBCASE("source 只认三个值") {
        llm::ReplayClient client({good_bible().dump()});
        pipeline::CancelToken tok;
        try {
            http::post_bible(
                json{{"project", p_str(root)}, {"source", "novel"}}, client, tok);
            FAIL("应该抛");
        } catch (const http::ApiError& e) {
            CHECK(e.status() == 422);
        }
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/bible：点名要故事但项目里没有") {
    const fs::path root = fresh_project("圣经没故事");
    llm::ReplayClient client({good_bible().dump()});
    pipeline::CancelToken tok;
    try {
        http::post_bible(json{{"project", p_str(root)}, {"source", "story"}},
                         client, tok);
        FAIL("应该抛");
    } catch (const http::ApiError& e) {
        CHECK(e.status() == 400);
    }
    // 一次模型都不该调
    CHECK(client.calls().empty());
    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---- 梗概不是必填的 ----
//
// 三个入口里只有「我自己有个想法」那条是从手写的一句话开始的；
// 给几个关键词、或者什么都不给让它来一个，同样正当。把梗概做成硬门槛
// 等于又把人摁回空白框前面发呆，而选题本来就是最难从零开始的一步。

TEST_CASE("没给梗概：提示词让模型自己定选题") {
    const std::string none =
        build_outline_prompt("", StoryScale::MEDIUM, StyleLine::REALISTIC);
    CHECK(none.find("这部剧讲什么**由你定**") != std::string::npos);
    // 「这部剧讲的是：」后面本来要跟梗概，没梗概时整段都不该出现
    CHECK(none.find("这部剧讲的是") == std::string::npos);

    const std::string with =
        build_outline_prompt("深夜便利店", StoryScale::MEDIUM, StyleLine::REALISTIC);
    CHECK(with.find("这部剧讲的是") != std::string::npos);
    CHECK(with.find("由你定") == std::string::npos);

    // 只给关键词也算「没给梗概」，方向照样带进去
    const std::string kw = build_outline_prompt("", StoryScale::MEDIUM,
                                                StyleLine::REALISTIC, "重生复仇");
    CHECK(kw.find("往这个方向想：重生复仇") != std::string::npos);
    CHECK(kw.find("由你定") != std::string::npos);
}

TEST_CASE("schema 里有 premise，给模型一个地方写它定的选题") {
    CHECK(outline_schema().at("properties").contains("premise"));
}

TEST_CASE("梗概谁说了算") {
    json j = good_outline();
    j["premise"] = "模型自己想的那个选题";

    SUBCASE("用户没给：收模型的") {
        const Story s = parse_outline(j.dump(), "", StoryScale::MEDIUM);
        CHECK(s.premise == "模型自己想的那个选题");
    }

    SUBCASE("用户给了：一个字不动，不让它改写") {
        const Story s = parse_outline(j.dump(), "用户写的那一句", StoryScale::MEDIUM);
        CHECK(s.premise == "用户写的那一句");
    }

    SUBCASE("两边都没有：空着，但别的照样解析得出来") {
        json empty = good_outline();
        const Story s = parse_outline(empty.dump(), "", StoryScale::MEDIUM);
        CHECK(s.premise.empty());
        CHECK(s.chapters.size() == 2);
    }
}

TEST_CASE("POST /api/story/outline：一个字都没有也照写") {
    const fs::path root = fresh_project("空梗概");
    json reply = good_outline();
    reply["premise"] = "模型自己想的那个选题";
    llm::ReplayClient client({reply.dump()});
    pipeline::CancelToken tok;

    // premise 不传、项目上也没有——原来这里是 400
    const auto r =
        http::post_story_outline(json{{"project", p_str(root)}}, client, tok);
    CHECK(r.status == 200);
    CHECK(r.body.at("story").at("premise").get<std::string>() ==
          "模型自己想的那个选题");
    REQUIRE(client.calls().size() == 1);
    CHECK(client.calls()[0].prompt.find("由你定") != std::string::npos);

    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---- 期 4：从故事写一集 ----

namespace {

/// 一份展开过正文的故事：两章各 1000 字，各带一个钩子。
Story written_story() {
    Story s = sample_story();
    for (int i = 0; i < 2; ++i) {
        Chapter& c = s.chapters[i];
        c.text.clear();
        for (int k = 0; k < 1000; ++k) c.text += (i == 0 ? "甲" : "乙");
        c.hooks.clear();
        Hook h;
        h.at_char = 500;
        h.text = i == 0 ? "她认出那把伞" : "他没有回头";
        c.hooks.push_back(h);
    }
    s.plan = changji::stages::plan_episodes(s, 30.0);
    return s;
}

}  // namespace

TEST_CASE("取一集覆盖的那段正文：按字符切，不按字节") {
    const Story s = written_story();
    REQUIRE(s.plan.size() >= 2);

    for (const auto& p : s.plan) {
        const std::string t = changji::stages::episode_text(s, p);
        // 切出来必须是完整的汉字。劈成半个的话字符数会对不上
        CHECK(t.size() % 3 == 0);
        CHECK_FALSE(t.empty());
    }

    // 第一集从头开始
    CHECK(s.plan[0].from_char == 0);
    // 相邻两集首尾相接，不重不漏
    for (std::size_t i = 1; i < s.plan.size(); ++i) {
        if (s.plan[i].from_chapter == s.plan[i - 1].to_chapter) {
            CHECK(s.plan[i].from_char == s.plan[i - 1].to_char);
        }
    }
}

TEST_CASE("上下文：前情是压缩过的，而且只到这一集之前") {
    Story s = written_story();
    // 手工造一条落在第二章的分集，好让前情里有东西
    EpisodePlan p;
    p.episode_id = "ep09";
    p.target_duration_s = 60.0;
    p.from_chapter = "ch02";
    p.from_char = 0;
    p.to_chapter = "ch02";
    p.to_char = 1000;
    p.hook = "他没有回头";

    const std::string ctx = changji::stages::render_script_context(s, p, "");

    // 前情提要在，而且是**每章一句的梗概**，不是正文原文
    CHECK(ctx.find("【前情提要】") != std::string::npos);
    CHECK(ctx.find("他推门进来") != std::string::npos);
    // 第一章的正文（一千个「甲」）不该整段搬进来
    CHECK(ctx.find("甲甲甲甲甲甲甲甲甲甲") == std::string::npos);

    // 这一集的正文在
    CHECK(ctx.find("【这一集】") != std::string::npos);
    CHECK(ctx.find("乙乙乙") != std::string::npos);

    // **停在哪**——这一条老路线完全没有
    CHECK(ctx.find("【这一集要停在】他没有回头") != std::string::npos);

    // 人物和关系是压缩的全局记忆
    CHECK(ctx.find("林晚") != std::string::npos);
    CHECK(ctx.find("前任") != std::string::npos);
}

TEST_CASE("第一集没有前情") {
    const Story s = written_story();
    const std::string ctx =
        changji::stages::render_script_context(s, s.plan[0], "");
    CHECK(ctx.find("【前情提要】") == std::string::npos);
}

TEST_CASE("上一集的结尾接得上，而且不从半行中间起") {
    const Story s = written_story();
    const std::string prev = "林晚：你还留着它。\n陈默把伞放在柜台上，没有说话。";
    const std::string ctx =
        changji::stages::render_script_context(s, s.plan[0], prev);
    CHECK(ctx.find("【上一集是这么结束的】") != std::string::npos);
    CHECK(ctx.find("陈默把伞放在柜台上") != std::string::npos);

    // 很长的剧本只取尾巴，而且从行首起
    std::string longer;
    for (int i = 0; i < 60; ++i) longer += "林晚：这是第 x 句台词。\n";
    longer += "陈默：最后一句。";
    const std::string tail = changji::stages::script_tail(longer);
    CHECK(tail.find("陈默：最后一句。") != std::string::npos);
    CHECK(tail.rfind("林晚：", 0) == 0);
}

TEST_CASE("提示词：这一集要发生什么已经定好了") {
    const Story s = written_story();
    const std::string p = changji::stages::build_script_prompt_from_story(
        s, s.plan[0], StyleLine::REALISTIC, {"林晚", "陈默"});

    // 和老路线的分水岭：不是让它构思剧情
    CHECK(p.find("已经定好了") != std::string::npos);
    CHECK(p.find("不要把前情再演一遍") != std::string::npos);
    CHECK(p.find("结尾必须停在给定的那个钩子上") != std::string::npos);
    // 时长按分集表来，字数预算跟着算
    CHECK(p.find("总时长约 30 秒") != std::string::npos);
    CHECK(p.find("必须沿用这些已有角色，名字一字不改：林晚、陈默") !=
          std::string::npos);
    // 动作要拍得出来那几条留着
    CHECK(p.find("写角色**身体在做什么**") != std::string::npos);
}

TEST_CASE("POST /api/story/episodes：把分集表落成真的剧集") {
    const fs::path root = fresh_project("落成剧集");
    ProjectStore store(root);
    const Story s = written_story();
    store.save_story(s);

    const auto r = http::post_story_episodes(json{{"project", p_str(root)}});
    CHECK(r.status == 200);
    CHECK(r.body.at("created").size() == s.plan.size());

    Project project = store.load_project();
    REQUIRE(project.episodes.size() == s.plan.size());
    const Episode& first = project.episodes[0];
    CHECK(first.episode_id == s.plan[0].episode_id);
    CHECK(first.target_duration_s == doctest::Approx(s.plan[0].target_duration_s));
    CHECK_FALSE(first.chapter_refs.empty());
    CHECK(first.chapter_refs[0] == "ch01");

    SUBCASE("再来一次：只补元数据，写好的剧本一个字不动") {
        Project p2 = store.load_project();
        p2.episodes[0].script = "林晚：这是已经写好的剧本。";
        store.save_project(p2);

        const auto again =
            http::post_story_episodes(json{{"project", p_str(root)}});
        CHECK(again.body.at("created").empty());
        CHECK(again.body.at("updated").size() == s.plan.size());
        CHECK(store.load_project().episodes[0].script ==
              "林晚：这是已经写好的剧本。");
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/episodes：还没有分集表") {
    const fs::path root = fresh_project("没分集表");
    CHECK_THROWS_AS(http::post_story_episodes(json{{"project", p_str(root)}}),
                    http::ApiError);
    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---- 粘贴导入：三种来源里的第二条 ----

TEST_CASE("认出作者自己分的章") {
    const std::string novel =
        "第一章 雨夜重逢\n"
        "他推门进来，伞还在手里。\n"
        "林晚认出了那把伞。\n"
        "第二章 五年前那把伞\n"
        "回到五年前的那个雨夜。\n"
        "第三章 便利店打烊\n"
        "卷帘门落下来。";

    const auto chs = changji::stages::split_pasted(novel);
    REQUIRE(chs.size() == 3);
    CHECK(chs[0].chapter_id == "ch01");
    CHECK(chs[0].title == "第一章 雨夜重逢");
    CHECK(chs[1].title == "第二章 五年前那把伞");
    // 标题行本身不进正文
    CHECK(chs[0].text.find("第一章") == std::string::npos);
    CHECK(chs[0].text.find("他推门进来") != std::string::npos);
    // 段落边界登记成候选切点，不然一整章只有章界一个候选
    CHECK_FALSE(chs[0].hooks.empty());
    // 钩子文本留空：段落边界不是真钩子，不编一句假的出来
    CHECK(chs[0].hooks[0].text.empty());
}

TEST_CASE("认标题：宁可漏认不要错认") {
    std::string t;
    CHECK(changji::stages::split_pasted("## 楔子\n正文一\n## 第一章\n正文二").size() == 2);

    // 正文里提到「第三章」的长句子不该被当成标题——错认会让那一章从
    // 半句话开始，而且章名是一整句废话
    const std::string tricky =
        "第一章\n"
        "他翻开那本书，第三章的页脚被人折过，折痕很深，像是反复读过很多遍。\n"
        "第二章\n"
        "她没有回答。";
    const auto chs = changji::stages::split_pasted(tricky);
    REQUIRE(chs.size() == 2);
    CHECK(chs[0].text.find("折痕很深") != std::string::npos);
}

TEST_CASE("一个标题都没有：按字数在段落边界上切") {
    // 六十段，每段约 100 字
    std::string novel;
    for (int i = 0; i < 60; ++i) {
        for (int k = 0; k < 100; ++k) novel += "字";
        novel += "\n";
    }

    const auto chs = changji::stages::split_pasted(novel, 1000);
    CHECK(chs.size() >= 4);

    for (const auto& c : chs) {
        // 切出来必须是完整的汉字
        CHECK(c.text.size() % 3 == 0);
        CHECK_FALSE(c.title.empty());
        CHECK_FALSE(c.text.empty());
    }

    // 一个字都不能丢。数「字」本身，不数换行——换行在章界上会被
    // strip_ws 削掉，算进来只会让这条断言变成在量空白。
    std::size_t kept = 0;
    for (const auto& c : chs) {
        for (const auto& ch : changji::text::utf8_chars(c.text)) {
            if (ch == "字") ++kept;
        }
    }
    CHECK(kept == 60 * 100);
}

TEST_CASE("切出来的章能直接拿去分集，而且切点落在段落上") {
    std::string novel;
    for (int i = 0; i < 40; ++i) {
        for (int k = 0; k < 100; ++k) novel += "字";
        novel += "\n";
    }
    Story s;
    s.chapters = changji::stages::split_pasted(novel, 2000);
    s.episode_duration_s = 60.0;  // 容量 900 字
    s.plan = changji::stages::plan_episodes(s, 60.0);

    CHECK(s.plan.size() >= 3);
    CHECK(s.validate().empty());

    // **每一刀要么落在章尾，要么紧跟在一个换行后面。**
    // 这是「不切在半句话中间」那条底线的可检查版本。
    //
    // 别写成 to_char % 100 == 0：每段是 100 字**加一个换行**，边界在 101
    // 的倍数上，而且章首被 strip_ws 削过之后偏移还会挪——用整除去凑，
    // 测的是算术不是那条性质。
    for (const auto& p : s.plan) {
        const Chapter* c = s.chapter_by_id(p.to_chapter);
        REQUIRE(c != nullptr);
        const auto chars = changji::text::utf8_chars(c->text);
        CAPTURE(p.episode_id);
        CAPTURE(p.to_char);
        const bool at_end = p.to_char == static_cast<int>(chars.size());
        const bool after_newline =
            p.to_char > 0 && p.to_char <= static_cast<int>(chars.size()) &&
            chars[static_cast<std::size_t>(p.to_char) - 1] == "\n";
        CHECK((at_end || after_newline));
    }
}

TEST_CASE("空的和切不出来的") {
    CHECK(changji::stages::split_pasted("").empty());
    CHECK(changji::stages::split_pasted("   \n  \n ").empty());
    // 一行字也算一章
    CHECK(changji::stages::split_pasted("就这一句。").size() == 1);
}

TEST_CASE("POST /api/story/import：只回草稿，不落库") {
    const fs::path root = fresh_project("粘贴");
    const std::string novel =
        "第一章 雨夜重逢\n他推门进来。\n第二章 五年前\n回到五年前。";

    const auto r = http::post_story_import(
        json{{"project", p_str(root)}, {"text", novel}});
    CHECK(r.status == 200);
    CHECK_FALSE(r.body.at("adopted").get<bool>());
    CHECK(r.body.at("chapters").get<int>() == 2);
    CHECK(r.body.at("written").get<int>() == 2);  // 粘进来的就是有正文的
    CHECK(r.body.at("story").at("source").get<std::string>() == "pasted");
    // 人物还提不出来，前端要靠这个数提醒人下一步
    CHECK(r.body.at("needs_analysis").get<bool>());

    ProjectStore store(root);
    CHECK(store.load_story().empty());

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/import：空文本") {
    const fs::path root = fresh_project("粘空的");
    CHECK_THROWS_AS(http::post_story_import(
                        json{{"project", p_str(root)}, {"text", "   "}}),
                    http::ApiError);
    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---- 读一遍现成的正文，把结构提出来 ----

namespace {

/// 一份粘进来、已经切好章的故事。
Story pasted_story() {
    Story s;
    s.source = StorySource::PASTED;
    const std::string novel =
        "第一章 雨夜重逢\n"
        "他推门进来，伞还在手里。\n"
        "林晚认出了那把伞。\n"
        "第二章 五年前那把伞\n"
        "回到五年前的那个雨夜。\n"
        "他没有回头。";
    s.chapters = changji::stages::split_pasted(novel);
    return s;
}

/// 老形状：单个 hook + hook_after。留一条用例盯着它还能读。
json good_analysis_legacy() {
    return json{
        {"logline", "一把伞牵出五年前的事"},
        {"genre", "都市情感"},
        {"tone", "克制"},
        {"characters",
         json::array({json{{"name", "林晚"},
                           {"identity", "便利店夜班店员"},
                           {"want", "把伞还回去"},
                           {"arc", "从躲到面对"}}})},
        {"relations", json::array()},
        {"locations",
         json::array({json{{"name", "便利店"}, {"what", "临街那家"}, {"when", "深夜"}}})},
        {"chapters",
         json::array({
             json{{"chapter_id", "ch01"},
                  {"summary", "他带着伞回来了。"},
                  {"hook", "她认出那把伞"},
                  {"hook_after", "他推门进来，伞还在手里。"},
                  {"characters", json::array({"林晚"})},
                  {"locations", json::array({"便利店"})}},
             json{{"chapter_id", "ch02"},
                  {"summary", "回到五年前。"},
                  {"hook", "他没有回头"},
                  {"hook_after", "回到五年前的那个雨夜。"}},
         })},
    };
}

/// 新形状：每章好几个钩子，各自带一句原文当锚。
json good_analysis() {
    json j = good_analysis_legacy();
    for (auto& c : j["chapters"]) {
        const std::string hook = c.value("hook", std::string());
        const std::string after = c.value("hook_after", std::string());
        c.erase("hook");
        c.erase("hook_after");
        c["hooks"] = json::array({json{{"text", hook}, {"after", after}}});
    }
    return j;
}

}  // namespace

TEST_CASE("节选：每章都在，中间省略要标出来") {
    Story s;
    for (int i = 0; i < 3; ++i) {
        Chapter c;
        c.chapter_id = "ch0" + std::to_string(i + 1);
        c.title = "第 " + std::to_string(i + 1) + " 章";
        c.text = "开头这一句。";
        for (int k = 0; k < 8000; ++k) c.text += "中";
        c.text += "结尾这一句。";
        s.chapters.push_back(c);
    }

    const std::string t = changji::stages::render_chapters_for_analysis(s);
    // 三章一章都不能少——漏掉一整章比每章少几百字糟糕得多
    for (int i = 1; i <= 3; ++i) {
        CHECK(t.find("ch0" + std::to_string(i)) != std::string::npos);
    }
    // 头尾都要，中间省略
    CHECK(t.find("开头这一句") != std::string::npos);
    CHECK(t.find("结尾这一句") != std::string::npos);
    CHECK(t.find("（中间略）") != std::string::npos);
    // 总量受控，不会把整本书塞进去
    CHECK(changji::text::utf8_len(t) < 20000);

    // 短章整章给，不省略
    Story small;
    Chapter c;
    c.chapter_id = "ch01";
    c.title = "短的";
    c.text = "就这么几个字。";
    small.chapters.push_back(c);
    const std::string t2 = changji::stages::render_chapters_for_analysis(small);
    CHECK(t2.find("（中间略）") == std::string::npos);
}

TEST_CASE("提示词：读，不要改写") {
    const Story s = pasted_story();
    const std::string p =
        changji::stages::build_analyze_prompt(s, StyleLine::REALISTIC);
    CHECK(p.find("不要改写正文") != std::string::npos);
    CHECK(p.find("只提文本里真实出现的东西") != std::string::npos);
    CHECK(p.find("不要写长相") != std::string::npos);
    CHECK(p.find("chapter_id 照抄") != std::string::npos);
    CHECK(p.find("ch01") != std::string::npos);
}

TEST_CASE("人物表里要有「他说话什么样」") {
    // **2026-09-12 加的，因为所有人说话都一个腔调。** 人物表里有身份、
    // 欲望、弧光，唯独没有「怎么开口」，于是正文里每个人的台词都像同一个
    // 人写的——而对白是短剧最主要的东西。
    const auto& cs = outline_schema().at("properties").at("characters").at("items");
    REQUIRE(cs.at("properties").contains("voice"));
    bool required = false;
    for (const auto& r : cs.at("required")) {
        if (r == "voice") required = true;
    }
    CHECK(required);
    // **required 只保证键在，不保证有内容。** 2026-09-12 实跑，加进
    // required 的头一轮三个人物的 voice 全是空串——空字符串是合法的
    // JSON 字符串，语法采样照样让它过，那一轮的改动一个字都没生效。
    CHECK(cs.at("properties").at("voice").at("minLength").get<int>() >= 6);

    // **「他怕什么」和「他要什么」是一对。** 短剧那边的说法是「爆款人设的
    // 核心驱动力不是欲望而是恐惧」，90% 的人设翻车死于「全能感」。
    REQUIRE(cs.at("properties").contains("fear"));
    CHECK(cs.at("properties").at("fear").at("minLength").get<int>() >= 6);
    bool fear_required = false;
    for (const auto& r : cs.at("required")) {
        if (r == "fear") fear_required = true;
    }
    CHECK(fear_required);
    // 挨着 want 填，模型才会让它们互相顶着
    std::vector<std::string> ck;
    for (auto it = cs.at("properties").begin(); it != cs.at("properties").end();
         ++it) {
        ck.push_back(it.key());
    }
    const auto pos = [&](const std::string& k) {
        return std::find(ck.begin(), ck.end(), k) - ck.begin();
    };
    CHECK(pos("fear") == pos("want") + 1);

    // 读故事那一步用的是同一份人物块，两边不一致的话每个消费者都要分支
    CHECK(changji::stages::analyze_schema().at("properties").at("characters") ==
          outline_schema().at("properties").at("characters"));
}

TEST_CASE("schema：每一章都要说清抖出什么") {
    // **2026-09-12 加的，因为四章零反转。** 实跑的大纲是「前任回来 → 打
    // 电话 → 坦白 → 和解」：每章都在推进，但没有一章让人重新理解前面发生
    // 过的事。爆款短剧每几集一个身份/关系/事实/动机的反转，网文那边叫
    // 「信息差」。措辞 14B 不一定听，进 required 它才没得选。
    const auto& ch = outline_schema().at("properties").at("chapters").at("items");
    REQUIRE(ch.at("properties").contains("reveal"));

    bool required = false;
    for (const auto& r : ch.at("required")) {
        if (r == "reveal") required = true;
    }
    CHECK(required);

    // **排在 summary 前面。** 先定抖什么，那几句梗概才会围着它写；反过来
    // 它会先把梗概写完，再回头凑一个「反转」，凑出来的是同一件事换个说法。
    std::vector<std::string> keys;
    const auto& props = ch.at("properties");
    for (auto it = props.begin(); it != props.end(); ++it) keys.push_back(it.key());
    const auto at = [&](const std::string& k) {
        return std::find(keys.begin(), keys.end(), k) - keys.begin();
    };
    CHECK(at("reveal") < at("summary"));
}

TEST_CASE("schema：人物那三块和大纲那份长一样") {
    const auto& a = changji::stages::analyze_schema().at("properties");
    const auto& o = outline_schema().at("properties");
    // 下游认的是同一个形状，两边不一致的话每个消费者都要分支
    CHECK(a.at("characters") == o.at("characters"));
    CHECK(a.at("relations") == o.at("relations"));
    CHECK(a.at("locations") == o.at("locations"));
    // 章名不给模型改——那是作者自己写的
    CHECK_FALSE(a.at("chapters").at("items").at("properties").contains("title"));
    // **每章要标好几个钩子，不是只标章尾。** 一章会切成好几集，只给章尾
    // 那一个的话前面几集只能收在无名的段落边界上——实跑时 12 集里只有 3 集
    // 停在真悬念上，就是这么来的。
    const auto& ch = a.at("chapters").at("items").at("properties");
    CHECK(ch.contains("hooks"));
    CHECK(ch.at("hooks").at("type") == "array");
    CHECK(ch.at("hooks").at("items").at("properties").contains("after"));
}

TEST_CASE("schema：正文一场一个数组，场数和段数都由语法卡住") {
    // 2026-09-11 实跑：正文只是一个 text 字符串时，14B 三章里两章把梗概原样
    // 抄进去就收工（一百来字），另一次写到 8192 token 都没收口。「写满三千
    // 字」它不听，进 schema 变成语法约束它才没得选。
    //
    // 2026-09-12 再改一层：**正文按场分组**。没有「场」这个单位的时候，
    // 模型把整章梗概平摊成四十个一句话的段落，通篇是概述不是场景——
    // 用户的判词是「只能叫剧本不能叫小说」。
    const auto s = changji::stages::chapter_schema(3, 22);
    const auto& props = s.at("properties");
    CHECK_FALSE(props.contains("text"));
    CHECK_FALSE(props.contains("paragraphs"));  // 正文不在顶层了
    REQUIRE(props.contains("scenes"));
    // **下限就是目标值。** 让它少写一场，那一场会长到一千八百字，分集只能
    // 在场中间连切四刀，每刀都落在说不出为什么的地方（2026-09-12 实跑）。
    CHECK(props.at("scenes").at("minItems").get<int>() == 3);
    CHECK(props.at("scenes").at("maxItems").get<int>() == 4);

    const auto& scene = props.at("scenes").at("items");
    const auto& sp = scene.at("properties");
    // **一场戏要素齐全**：在哪、跟谁走、要什么、谁拦着、局面变成什么。
    for (const char* k : {"where", "pov", "goal", "obstacle", "turn"}) {
        CHECK(sp.contains(k));
    }
    // **turn 排在正文前面**：先知道这一场停在哪，才写得到那儿去。
    auto keys = std::vector<std::string>();
    for (auto it = sp.begin(); it != sp.end(); ++it) keys.push_back(it.key());
    const auto pos = [&](const std::string& k) {
        return std::find(keys.begin(), keys.end(), k) - keys.begin();
    };
    CHECK(pos("where") < pos("paragraphs"));
    CHECK(pos("turn") < pos("paragraphs"));

    REQUIRE(sp.contains("paragraphs"));
    CHECK(sp.at("paragraphs").at("type") == "array");
    CHECK(sp.at("paragraphs").at("minItems").get<int>() >= 12);
    CHECK(sp.at("paragraphs").at("maxItems").get<int>() <= 34);
    CHECK(sp.at("paragraphs").at("items").at("type") == "string");
    REQUIRE(s.at("required").size() == 1);
    CHECK(s.at("required")[0] == "scenes");

    // 目标再小，下限也不会低到能一段交差、一场交差
    const auto tiny = changji::stages::chapter_schema(1, 2);
    CHECK(tiny.at("properties").at("scenes").at("minItems").get<int>() >= 2);
    CHECK(tiny.at("properties").at("scenes").at("items").at("properties")
              .at("paragraphs").at("minItems").get<int>() >= 6);
}

TEST_CASE("并回去：每章那两份名单是重填，不是往上堆") {
    // **2026-09-11 实跑出来的样子**：走完"出大纲 → 写正文 → 读故事"，
    // 某一章的出场人物是 `['陈默','林景明','陈默','林景明','苏婉']`，
    // 地点那份还混着同一个地方的两种叫法。
    //
    // 两条原因：大纲那一步已经往里写过一份，而这儿只 push 不 clear；
    // 模型自己也会把同一个名字写两遍。
    //
    // 读一遍正文**是重读，不是补充**：正文改过之后，上一次读出来的名单
    // 本来就整份作废。留着只会让分镜提示词里同一个人名出现两次、同一个
    // 地方指向两条不同的设定。
    Story s = pasted_story();
    // 假装大纲那一步已经填过（真实流程就是这样）
    s.chapters[0].characters = {"林晚", "旧的名字"};
    s.chapters[0].locations = {"便利店", "旧的地方"};

    json j = good_analysis();
    // 模型把同一个人写了两遍——它真会这么干
    j["chapters"][0]["characters"] = json::array({"林晚", "林晚"});
    j["chapters"][0]["locations"] = json::array({"便利店", "便利店"});

    const Story got = changji::stages::apply_analysis(s, j.dump());
    CHECK(got.chapters[0].characters == std::vector<std::string>{"林晚"});
    CHECK(got.chapters[0].locations == std::vector<std::string>{"便利店"});
}

TEST_CASE("并回去：模型没给关系时不要把已有的抹掉") {
    // relations 一度不在 required 里，模型给的是空数组，而这儿会拿它盖掉
    // 大纲写好的那几条——界面上看着像"这个故事没有人物关系"。
    // schema 那条已经补上了（见「地点和出场人要写进 required」），
    // 这一条守的是并回去这一步：给了空数组，至少别比原来更糟。
    Story s = pasted_story();
    Relation r;
    r.a = "林晚";
    r.b = "他";
    r.kind = "前任";
    r.tension = "五年前那把伞";
    s.relations.push_back(r);

    json j = good_analysis();
    j["relations"] = json::array();   // 模型什么都没给
    const Story got = changji::stages::apply_analysis(s, j.dump());
    // 空数组进来时，两端还在不在人物表里都无从判断——这里只钉一件事：
    // 结果不该比原来更糟。
    CHECK(got.relations.size() <= s.relations.size());
}

TEST_CASE("并回去：正文一个字不动，钩子落在那句话后面") {
    const Story s = pasted_story();
    const std::string before = s.chapters[0].text;

    const Story got =
        changji::stages::apply_analysis(s, good_analysis().dump());

    // 正文、章名、章号照旧
    CHECK(got.chapters[0].text == before);
    CHECK(got.chapters[0].title == s.chapters[0].title);
    CHECK(got.chapters[0].chapter_id == "ch01");

    // 结构补上了
    CHECK(got.logline == "一把伞牵出五年前的事");
    REQUIRE(got.characters.size() == 1);
    CHECK(got.characters[0].name == "林晚");
    CHECK(got.chapters[0].summary == "他带着伞回来了。");

    // 钩子定位：落在 hook_after 那句话**之后**
    bool found = false;
    for (const auto& h : got.chapters[0].hooks) {
        if (h.text != "她认出那把伞") continue;
        found = true;
        const auto chars = changji::text::utf8_chars(got.chapters[0].text);
        REQUIRE(h.at_char > 0);
        REQUIRE(h.at_char <= static_cast<int>(chars.size()));
        CHECK(chars[static_cast<std::size_t>(h.at_char) - 1] == "。");
    }
    CHECK(found);

    CHECK(got.validate().empty());
}

TEST_CASE("并回去：模型糊弄时的几种情况") {
    const Story s = pasted_story();

    SUBCASE("after 查不到——那一条丢掉，不能都堆到章尾") {
        // 一章有好几个钩子，查不到的全往章尾堆的话，章尾会被一个中间情节的
        // 说法占掉，而那一集的结尾写的就是别处的事。
        json j = good_analysis();
        j["chapters"][0]["hooks"][0]["after"] = "正文里根本没有这句话";
        const Story got = changji::stages::apply_analysis(s, j.dump());
        for (const auto& h : got.chapters[0].hooks) {
            CHECK(h.text != "她认出那把伞");
        }
        CHECK(got.validate().empty());
    }

    SUBCASE("老形状（单个 hook + hook_after）还能读，查不到时兜底挂章尾") {
        json j = good_analysis_legacy();
        j["chapters"][0]["hook_after"] = "正文里根本没有这句话";
        const Story got = changji::stages::apply_analysis(s, j.dump());
        bool found = false;
        for (const auto& h : got.chapters[0].hooks) {
            if (h.text == "她认出那把伞") {
                found = true;
                CHECK(h.at_char == got.chapters[0].text_len());
            }
        }
        CHECK(found);
        CHECK(got.validate().empty());
    }

    SUBCASE("一章标好几个钩子，各就各位") {
        json j = good_analysis();
        j["chapters"][0]["hooks"] = json::array({
            json{{"text", "他进来了"}, {"after", "他推门进来，伞还在手里。"}},
            json{{"text", "她认出那把伞"}, {"after", "林晚认出了那把伞。"}},
        });
        const Story got = changji::stages::apply_analysis(s, j.dump());
        int named = 0;
        for (const auto& h : got.chapters[0].hooks) {
            if (!h.text.empty()) ++named;
        }
        // 两个都落下去了——这正是「12 集只有 3 集停在真悬念上」要修的地方
        CHECK(named == 2);
        CHECK(got.validate().empty());
    }

    SUBCASE("编了个不存在的章号——跳过，别把它当新章") {
        json j = good_analysis();
        j["chapters"].push_back(json{{"chapter_id", "ch99"},
                                     {"summary", "查无此章"},
                                     {"hooks", json::array()}});
        const Story got = changji::stages::apply_analysis(s, j.dump());
        CHECK(got.chapters.size() == s.chapters.size());
        CHECK(got.validate().empty());
    }

    SUBCASE("章节里冒出没登记的人——过滤掉") {
        json j = good_analysis();
        j["chapters"][0]["characters"].push_back("查无此人");
        const Story got = changji::stages::apply_analysis(s, j.dump());
        for (const auto& n : got.chapters[0].characters) {
            CHECK(n == "林晚");
        }
    }

    SUBCASE("一个人都没读出来——报错，不要产出一份没人的故事") {
        json j = good_analysis();
        j["characters"] = json::array();
        CHECK_THROWS_AS(changji::stages::apply_analysis(s, j.dump()),
                        stages::StoryError);
    }
}

TEST_CASE("POST /api/story/analyze：只回草稿，而且重算了分集") {
    const fs::path root = fresh_project("读一遍");
    ProjectStore store(root);
    Story s = pasted_story();
    s.plan = changji::stages::plan_episodes(s, 60.0);
    store.save_story(s);

    llm::ReplayClient client({good_analysis().dump()});
    pipeline::CancelToken tok;
    const auto r =
        http::post_story_analyze(json{{"project", p_str(root)}}, client, tok);

    CHECK(r.status == 200);
    CHECK_FALSE(r.body.at("adopted").get<bool>());
    CHECK_FALSE(r.body.at("needs_analysis").get<bool>());
    CHECK(r.body.at("story").at("characters").size() == 1);
    // 盘上还是原来那份
    CHECK(store.load_story().characters.empty());

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/analyze：没正文可读") {
    const fs::path root = fresh_project("没正文");
    ProjectStore store(root);
    // 大纲写出来的故事本来就带人物表，不用走这一步
    store.save_story(parse_outline(good_outline().dump(), "梗概", StoryScale::MEDIUM));

    llm::ReplayClient client({good_analysis().dump()});
    pipeline::CancelToken tok;
    CHECK_THROWS_AS(
        http::post_story_analyze(json{{"project", p_str(root)}}, client, tok),
        http::ApiError);
    CHECK(client.calls().empty());

    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---- 逐章展开正文 ----

namespace {

/// 一段够长的正文，开头带个记号好认。
///
/// 走 /api/story/chapter 那几条要过 kChapterMinRatio 的闸门（目标三千字的
/// 五分之一 = 600 字），十几个字的夹具会被当成"模型没写"顶回来——那正是
/// 闸门该干的事，所以夹具要写够长，不是把闸门调松。
/// 一段够长、而且**不重复**的正文。
///
/// 原来是 `mark + 700 个"字"`。复读守卫上线之后那份语料一律判废——而它
/// 判得对：700 个一模一样的字本来就是复读机。这是第二次栽在同一件事上，
/// 上一次是字数下限。**守卫抓到的是语料，那就改语料**：把合成的正文做成
/// 真的不重样，否则用例证明的只是"我们能绕过自己的守卫"。
std::string long_body(const std::string& mark) {
    static const char* kWho[] = {"林然", "沈悠", "陈默", "老板娘"};
    static const char* kDo[] = {"推开玻璃门", "把伞收起来", "看了一眼钟",
                                "拿起柜台上的杯子", "转身走向货架",
                                "停在雨里没有动"};
    static const char* kHow[] = {"雨声压过了店里的音乐", "灯管闪了一下",
                                 "他没有回头", "空气里有泡面的味道",
                                 "外面的车灯扫过墙面", "谁也没有先开口"};
    std::string s = mark;
    for (int i = 0; s.size() < 2400; ++i) {
        s += kWho[i % 4];
        s += kDo[(i * 3 + 1) % 6];
        s += "，";
        s += kHow[(i * 5 + 2) % 6];
        s += "，第" + std::to_string(i) + "次。\n\n";
    }
    return s;
}

/// 模型写回来的一章：正文 + 几个可以收一集的地方。
json good_chapter(const std::string& body,
                  const std::vector<std::pair<std::string, std::string>>& hooks = {}) {
    json hs = json::array();
    for (const auto& [why, after] : hooks) {
        hs.push_back(json{{"text", why}, {"after", after}});
    }
    return json{{"text", body}, {"hooks", hs}};
}

/// 大纲写出来的故事：有梗概有钩子，没有正文。
Story outline_only_story() {
    return parse_outline(good_outline().dump(), "深夜便利店", StoryScale::MEDIUM);
}

}  // namespace

TEST_CASE("一场的篇幅跟着每集时长走") {
    // **一场大致对着一集**，那一集的结尾正好是这场戏演完的地方。
    // 写死一千字的时候，每集 30 秒那一档里一场横跨快两集，分集只能在场
    // 中间下刀——2026-09-12 实跑，停在场尾从 86% 掉到 63%。
    Story s = outline_only_story();

    for (double d : {30.0, 60.0, 120.0, 300.0}) {
        s.episode_duration_s = d;
        CAPTURE(d);
        const int per_ep = changji::stages::prose_budget_chars(d);
        const int per_scene = changji::stages::scene_target_chars(s);

        // 夹在 600 和 1500 之间：短了写不成一场戏，长了一场横跨好几集
        CHECK(per_scene >= changji::stages::kSceneMinChars);
        CHECK(per_scene <= changji::stages::kSceneMaxChars);
        // 落在那个区间里的时长，一场就是一集
        if (per_ep >= changji::stages::kSceneMinChars &&
            per_ep <= changji::stages::kSceneMaxChars) {
            CHECK(per_scene == per_ep);
        }

        // 一章的场数是「一章多少字 ÷ 一场多少字」，夹在 2~5
        const int n = changji::stages::chapter_target_scenes(s);
        CHECK(n >= 2);
        CHECK(n <= 5);
    }

    // 每集短的时候要多切几场才跟得上；每集长的时候场数回落
    s.episode_duration_s = 30.0;
    const int many = changji::stages::chapter_target_scenes(s);
    s.episode_duration_s = 120.0;
    CHECK(changji::stages::chapter_target_scenes(s) <= many);
}

TEST_CASE("一章该写多长：章是故事单元，不是一集") {
    Story s = outline_only_story();

    // **一章至少要切得出好几集**，否则分集算法就没活干了。
    // 这条是端到端实跑时用户指出来的：早先按「它要撑起几集 × 每集容量」
    // 算，而大纲阶段一章一集，于是每章正好写一集的量，四章切出来正好四集。
    for (double d : {30.0, 60.0, 90.0, 180.0}) {
        s.episode_duration_s = d;
        s.plan = changji::stages::plan_episodes(s, d);
        const int target = changji::stages::chapter_target_chars(s);
        const int cap = changji::stages::prose_budget_chars(d);
        CAPTURE(d);
        CHECK(target >= cap * changji::stages::kEpisodesPerChapter);
        CHECK(target >= changji::stages::kChapterTargetChars);
    }

    // 分集表里排了几集不影响章的篇幅——章的长短是故事的事，不是时长的事
    s.episode_duration_s = 60.0;
    s.plan.clear();
    const int no_plan = changji::stages::chapter_target_chars(s);
    s.plan = changji::stages::plan_episodes(s, 60.0);
    CHECK(changji::stages::chapter_target_chars(s) == no_plan);
}

TEST_CASE("写满一章的量，就该切出好几集") {
    Story s = outline_only_story();
    s.episode_duration_s = 30.0;

    // 一章写到基准篇幅（段落边界当候选切点）
    // **每一行都要不一样。** 三十行一模一样的一百个「字」是复读机，
    // 复读守卫判得对；这里要的只是"够长、段落边界够多"。
    std::string body;
    for (int i = 0; i < 30; ++i) {
        body += "第" + std::to_string(i) + "段：";
        for (int k = 0; k < 96; ++k) body += "字";
        body += "\n";
    }
    s = changji::stages::apply_chapter(
        s, "ch01",
        changji::stages::parse_chapter(json{{"text", body}}.dump()));
    s.plan = changji::stages::plan_episodes(s, 30.0);

    int from_ch01 = 0;
    for (const auto& p : s.plan) {
        if (p.from_chapter == "ch01") ++from_ch01;
    }
    CHECK(from_ch01 >= 3);
}

TEST_CASE("提示词：只写这一章，带的是压缩的全局记忆") {
    Story s = outline_only_story();
    s.chapters[0].text = "第一章已经写好的正文。他推门进来。";

    const std::string p = changji::stages::build_chapter_prompt(
        s, "ch02", StyleLine::REALISTIC);

    // 分界线：只写这一章
    CHECK(p.find("只写这一章") != std::string::npos);
    // 2026-09-12：写作单位是「场」，不是一堆段落。没有这个单位的时候模型
    // 把整章梗概平摊成一串镜头，写出来是概述不是场景。
    CHECK(p.find("**一场戏是**") != std::string::npos);
    CHECK(p.find("写场面，不写概述") != std::string::npos);
    CHECK(p.find("一段推进的是**一两秒钟的事**") != std::string::npos);
    CHECK(p.find(std::to_string(changji::stages::chapter_target_scenes(s)) +
                 " 场戏") != std::string::npos);
    CHECK(p.find(std::to_string(changji::stages::chapter_scene_chars(s)) +
                 " 字上下") != std::string::npos);
    // 每一场停在自己的 turn 上，那就是一集的收口
    CHECK(p.find("每一场停在它的 turn 上") != std::string::npos);
    CHECK(p.find("最后一场的 turn 要落到这件事上") != std::string::npos);
    // 不许贴情绪标签，但要写内心：拆成身体和当下那句心里话
    CHECK(p.find("神情复杂") != std::string::npos);
    CHECK(p.find("一场只跟着一个人走") != std::string::npos);
    // 长相归美术那一步，但**身体要在场上**——上一版这条被模型扩大成
    // 「不要描写人」，人物在场景里没有身体
    CHECK(p.find("不要写长相") != std::string::npos);
    CHECK(p.find("身体要在场上") != std::string::npos);
    // 五感里至少有一个不靠眼睛
    CHECK(p.find("不靠眼睛") != std::string::npos);
    CHECK(p.find("从上一章停下的地方接着走") != std::string::npos);
    CHECK(p.find("【这是第一章】") == std::string::npos);

    // 压缩的全局记忆：人物、关系在，前情是每章一句
    CHECK(p.find("林晚") != std::string::npos);
    CHECK(p.find("前任") != std::string::npos);
    CHECK(p.find("【前情提要】") != std::string::npos);
    CHECK(p.find("他推门进来，伞还在手里") != std::string::npos);  // ch01 的梗概

    // 上一章结尾接语气
    CHECK(p.find("【上一章是这么结束的】") != std::string::npos);
    CHECK(p.find("第一章已经写好的正文") != std::string::npos);

    // 这一章要写什么、停在哪
    CHECK(p.find("【这一章】五年前那把伞") != std::string::npos);
    CHECK(p.find("【最后一场的 turn 要落到这件事上】他没有回头") !=
          std::string::npos);

    // 第一章没有前情，也没有上一章
    const std::string first = changji::stages::build_chapter_prompt(
        s, "ch01", StyleLine::REALISTIC);
    CHECK(first.find("【前情提要】") == std::string::npos);
    CHECK(first.find("【上一章是这么结束的】") == std::string::npos);
    // 第一章那个位置不能空着：实跑时 14B 两次都只写出一百来个字
    CHECK(first.find("【这是第一章】") != std::string::npos);
    // **第一章还要专门催对白。** 实测它的对白比例总是全书最低（14%、8%、
    // 20%，而后三章都在 27%~48%）——没有前情可接，模型就一个人在那儿看
    // 和想。读者认人靠的是听他们说话。
    CHECK(first.find("开头几段之内就要有人开口") != std::string::npos);
    CHECK(first.find("怎么称呼对方") != std::string::npos);
}

TEST_CASE("并回去：场的位置是数出来的，不是模型报的") {
    // **上一版靠模型抄一句原文回来（hooks[].after），程序再去正文里查。**
    // 抄错一个字那一集就落不下去，只能收在一个说不出为什么的段落边界上。
    // 现在正文是一场一场写的，第几段结束就是第几场结束——程序自己数。
    Story s = outline_only_story();

    const std::string a1 = "他把伞立在门边，水顺着伞骨往下淌。";
    const std::string a2 = "收银台那台关东煮机器在响，热气糊住了玻璃。";
    const std::string b1 = "天台的风比楼下大，铁门在身后合上。";
    const std::string b2 = "她没回头，手指扣着栏杆上那道缺口。";

    const json draft = {
        {"scenes",
         {{{"where", "深夜，便利店，只有冷柜的白光"},
           {"pov", "林晚"},
           {"goal", "把伞要回来"},
           {"obstacle", "他不认这把伞"},
           {"turn", "伞柄上刻着的不是她的名字"},
           {"paragraphs", {a1, a2}}},
          {{"where", "凌晨，楼顶天台，天还没亮"},
           {"pov", "林晚"},
           {"goal", "问清楚那个名字"},
           {"obstacle", "他一句话都不说"},
           {"turn", "他把伞从天台扔了下去"},
           {"paragraphs", {b1, b2}}}}}};

    const auto d = changji::stages::parse_chapter(draft.dump());
    REQUIRE(d.scenes.size() == 2);
    s = changji::stages::apply_chapter(s, "ch01", d);

    const Chapter* c = s.chapter_by_id("ch01");
    REQUIRE(c != nullptr);
    REQUIRE(c->scenes.size() == 2);

    // 正文就是各场的段落顺次拼起来的，段间一个换行
    CHECK(c->text == a1 + "\n" + a2 + "\n" + b1 + "\n" + b2);

    // 第一场收在第二段之后那个位置；两场首尾相接，末场顶到章尾
    const int first_end = static_cast<int>(
        text::utf8_len(a1 + "\n" + a2 + "\n"));
    CHECK(c->scenes[0].from_char == 0);
    CHECK(c->scenes[0].to_char == first_end);
    CHECK(c->scenes[1].from_char == first_end);
    CHECK(c->scenes[1].to_char == c->text_len());

    // 场的底子留着：写剧本那一步要知道这一集在哪、跟谁走
    CHECK(c->scenes[0].pov == "林晚");
    CHECK(c->scenes[0].where.find("便利店") != std::string::npos);

    // **每一场的末尾都成了有说法的切点**，而说法是**那一场的收尾那一句**，
    // 不是 turn。turn 老写成在脑子里发生的事（「他意识到当年误解了真相」），
    // 而且一章里两场常常是同一件事换个说法；收尾那一句被 last_line 那一栏
    // 约束成「turn 发生的那一刻」，必然是动作或台词，而且场场不同。
    bool at_first = false, at_end = false;
    for (const auto& h : c->hooks) {
        if (h.at_char == first_end && h.text == a2) at_first = true;
        if (h.at_char == c->text_len() && h.text == b2) at_end = true;
    }
    CHECK(at_first);
    CHECK(at_end);
    // turn 照旧存在场次表里：写剧本那一步拿得到
    CHECK(c->scenes[0].turn == "伞柄上刻着的不是她的名字");
}

TEST_CASE("并回去：老形状还认（顶层 paragraphs、顶层 text）") {
    // 改 schema 之前存下来的草稿、粘贴导入那条路都走这儿。认不出来的话
    // 那些故事一打开正文就是空的。
    Story s = outline_only_story();
    const json older = {{"paragraphs", {"他推门进来的时候，风也跟着进来了。",
                                        "她没有抬头，手里的杯子还冒着热气。"}}};
    s = changji::stages::apply_chapter(
        s, "ch01", changji::stages::parse_chapter(older.dump()));
    const Chapter* c = s.chapter_by_id("ch01");
    REQUIRE(c != nullptr);
    CHECK(c->text_len() > 20);
    CHECK(c->scenes.empty());  // 没有场就是没有，分集退回按段落边界切
}

TEST_CASE("正文在贴情绪标签就打回") {
    // 实跑那一章（chapter_check8 / ch02）1674 个字里，「神情复杂」
    // 「眼中满是惊讶与疑问」「心中涌起难以言喻的情绪」这类说法出现了十几次。
    // 它们把感受替读者做完了，是那份正文读起来像分镜表的主要原因之一。
    json scenes = json::array();
    json paras = json::array();
    for (int i = 0; i < 12; ++i) {
        paras.push_back("第" + std::to_string(i) +
                        "段：她站在那里，神情复杂地看着他走远。");
    }
    scenes.push_back({{"where", "深夜，便利店"},
                      {"pov", "林晚"},
                      {"goal", "要回伞"},
                      {"obstacle", "他不认"},
                      {"turn", "伞柄上刻着别人的名字"},
                      {"paragraphs", paras}});
    CHECK_THROWS_AS(
        changji::stages::parse_chapter(json{{"scenes", scenes}}.dump()),
        changji::stages::StoryError);

    // 偶尔冒一个不算：阈值是每千字三个，整章至少四个才判。
    json ok_paras = json::array();
    for (int i = 0; i < 12; ++i) {
        ok_paras.push_back("第" + std::to_string(i) +
                           "段：她把杯子放下，水在桌面上洇出一圈。");
    }
    ok_paras.push_back("他没说话，神情复杂地看了她一眼，然后推门出去了。");
    // 每场都要有对白，否则先撞上另一道闸
    ok_paras.push_back("她把抹布搭在台面上：“伞放那儿吧。”");
    json one = json::array();
    one.push_back({{"where", "深夜，便利店"},
                   {"pov", "林晚"},
                   {"goal", "要回伞"},
                   {"obstacle", "他不认"},
                   {"turn", "伞柄上刻着别人的名字"},
                   {"paragraphs", ok_paras}});
    CHECK_NOTHROW(changji::stages::parse_chapter(json{{"scenes", one}}.dump()));
}

TEST_CASE("在场的人是数组，语法上就不能只写一个") {
    // 上一版是一个字符串加 minLength，模型填了「林夏, 无他人」就绕过去了
    // ——2026-09-12 实跑，那一章对白只有 4%。措辞拦不住的用语法拦：数组的
    // minItems 进 GBNF 是硬的。这和第一轮把正文从 text 改成 paragraphs
    // 是同一招。
    const auto s = changji::stages::chapter_schema(3, 22);
    const auto& sp = s.at("properties").at("scenes").at("items").at("properties");
    REQUIRE(sp.contains("who"));
    CHECK(sp.at("who").at("type") == "array");
    CHECK(sp.at("who").at("minItems").get<int>() >= 2);

    // 落库之后是一行顿号分隔的名字
    json paras = json::array();
    for (int i = 0; i < 14; ++i) {
        paras.push_back("第" + std::to_string(i) +
                        "段：她把抹布拧干，水滴落在地板上，溅出细小的一圈。");
    }
    paras.push_back("他停在门口：“伞我带来了。”");
    paras.push_back("她没抬头：“放那儿吧。”");
    json scenes = json::array();
    scenes.push_back({{"where", "深夜，便利店"},
                      {"pov", "林晚"},
                      {"who", json::array({"林晚", "陈默"})},
                      {"goal", "把伞要回来"},
                      {"obstacle", "他不认这把伞"},
                      {"worse", "她发现伞根本不是他带来的"},
                      {"turn", "伞柄上刻着别人的名字"},
                      {"paragraphs", paras},
                      {"last_line", "她把伞柄转过来，刻着的不是她的名字。"}});
    const auto d = changji::stages::parse_chapter(json{{"scenes", scenes}}.dump());
    REQUIRE(d.scenes.size() == 1);
    CHECK(d.scenes[0].who == "林晚、陈默");
}

TEST_CASE("整章几乎没对白就打回") {
    // 门槛从「整章两处」提到「至少一成的段落有人说话」：两处那个下限太松，
    // 三跑基线里每一跑都有一章掉到个位数（4%、1%、25%），而它们都过了
    // 两处那道闸。那种章不是没人在场，是模型把对话整段转述掉了。
    const auto make = [](int spoken) {
        json paras = json::array();
        for (int i = 0; i < 30; ++i) {
            paras.push_back("第" + std::to_string(i) +
                            "段：她把抹布拧干，水滴落在地板上，溅出细小的一圈。");
        }
        for (int i = 0; i < spoken; ++i) {
            paras.push_back("他停在门口，手扶着门框：“伞我带来了" +
                            std::to_string(i) + "。”");
        }
        json scenes = json::array();
        scenes.push_back({{"where", "深夜，便利店"},
                          {"pov", "林晚"},
                          {"who", json::array({"林晚", "陈默"})},
                          {"goal", "把伞要回来"},
                          {"obstacle", "他不认这把伞"},
                          {"worse", "她发现伞根本不是他带来的"},
                          {"turn", "伞柄上刻着别人的名字"},
                          {"paragraphs", paras},
                          {"last_line", "她把伞柄转过来，刻着的不是她的名字。"}});
        return json{{"scenes", scenes}}.dump();
    };
    // 32 段里两处对白 = 6%，过了老闸，过不了新闸
    CHECK_THROWS_AS(changji::stages::parse_chapter(make(2)),
                    changji::stages::StoryError);
    // 一成够了
    CHECK_NOTHROW(changji::stages::parse_chapter(make(4)));
    // 软闸：最后一次尝试照收
    CHECK_NOTHROW(changji::stages::parse_chapter(make(2), 0, false));
}

TEST_CASE("分镜的话按小句摘掉，不摘整段") {
    // 「镜头拉远」「画面渐暗」是分镜的语言不是小说的语言，而后面另有一步
    // 专门把正文变成拍子。提示词里写了不要这么写，但 2026-09-12 量方差
    // 那两跑里一跑干净、另一跑又冒出来——规则不是没写，是在方差里时有
    // 时无。
    const auto body = [](const std::string& line) {
        json paras = json::array();
        for (int i = 0; i < 14; ++i) {
            paras.push_back("第" + std::to_string(i) +
                            "段：她把抹布拧干，水滴落在地板上，溅出细小的一圈。");
        }
        paras.push_back("他停在门口，手扶着门框：“伞我带来了。”");
        paras.push_back("她没抬头，抹布又抹了一遍：“放那儿吧。”");
        paras.push_back(line);
        json scenes = json::array();
        scenes.push_back({{"where", "深夜，便利店，冷柜的白光"},
                          {"pov", "林晚"},
                          {"who", json::array({"林晚", "陈默"})},
                          {"goal", "把伞要回来"},
                          {"obstacle", "他不认这把伞"},
                          {"worse", "她发现伞根本不是他带来的"},
                          {"turn", "伞柄上刻着别人的名字"},
                          {"paragraphs", paras},
                          {"last_line", "她把伞柄转过来，刻着的不是她的名字。"}});
        return json{{"scenes", scenes}}.dump();
    };

    // 前半句是正经正文，整段丢掉就把内容一起丢了
    const auto d = changji::stages::parse_chapter(
        body("纸条落在收银台的一角，镜头定格在上面那行字。"));
    CHECK(d.text.find("纸条落在收银台的一角") != std::string::npos);
    CHECK(d.text.find("镜头定格") == std::string::npos);

    // **单字不收。** 照片的画面、摄影机的镜头都是实物，不能误伤
    const auto keep = changji::stages::parse_chapter(
        body("照片的画面已经褪色，边角卷起一块，她用指甲压平。"));
    CHECK(keep.text.find("照片的画面已经褪色") != std::string::npos);

    // 摘完剩不下什么就还回原样：宁可留一句分镜话，也别把一段摘成半截
    const auto whole = changji::stages::parse_chapter(body("画面渐暗。"));
    CHECK(whole.text.find("画面渐暗") != std::string::npos);
}

TEST_CASE("一段里只有右引号就补回左引号") {
    // 2026-09-12 实跑：最后一集的钩子是「苏妍点头微笑。”好的。”」——两个
    // 都是右引号。normalize_quotes 只在整章没有 “ 时才动手，而这一章别处
    // 是正常的，所以这一段漏过去了，原样落进正文、落进分集的钩子、落进
    // 字幕。
    const auto body = [](const std::string& line) {
        json paras = json::array();
        for (int i = 0; i < 14; ++i) {
            paras.push_back("第" + std::to_string(i) +
                            "段：她把抹布拧干，水滴落在地板上，溅出细小的一圈。");
        }
        paras.push_back("他停在门口，手扶着门框：“伞我带来了。”");
        paras.push_back("她没抬头，抹布又抹了一遍：“放那儿吧。”");
        paras.push_back(line);
        json scenes = json::array();
        scenes.push_back({{"where", "深夜，便利店，冷柜的白光"},
                          {"pov", "林晚"},
                          {"who", json::array({"林晚", "陈默"})},
                          {"goal", "把伞要回来"},
                          {"obstacle", "他不认这把伞"},
                          {"worse", "她发现伞根本不是他带来的"},
                          {"turn", "伞柄上刻着别人的名字"},
                          {"paragraphs", paras},
                          {"last_line", "她把伞柄转过来，刻着的不是她的名字。"}});
        return json{{"scenes", scenes}}.dump();
    };

    const auto fixed = changji::stages::parse_chapter(
        body("苏妍点头微笑。”好的。”"));
    CHECK(fixed.text.find("“好的。”") != std::string::npos);

    // **引号跨段的写法不碰**：一个人连说几段，中间那几段只有右引号是正当的。
    // 这里右引号是奇数个，不动。
    const auto keep = changji::stages::parse_chapter(
        body("她顿了顿，接着说下去。”这些年我一直在等。"));
    CHECK(keep.text.find("”这些年我一直在等。") != std::string::npos);
}

TEST_CASE("每一场都要说清局面更糟在哪儿") {
    // **2026-09-12 加的，因为一章三场原地打转。** 实跑那一章三场都在同一个
    // 地方对着同一样东西，三个收尾是同一个手势的变奏（手指僵在半空 /
    // 手指在伞柄上方停住 / 指尖即将碰到又缩回），切出来三集的钩子长得一样。
    //
    // 编剧的老规矩是「通过事情的扭转，使情况比这场戏刚开始时更加恶劣」，
    // 短剧那边叫「每一集都要有信息增量」。局面更糟这件事没法重复三遍。
    const auto s = changji::stages::chapter_schema(3, 22);
    const auto& sp = s.at("properties").at("scenes").at("items").at("properties");
    REQUIRE(sp.contains("worse"));
    CHECK(sp.at("worse").at("minLength").get<int>() >= 6);

    bool required = false;
    for (const auto& r :
         s.at("properties").at("scenes").at("items").at("required")) {
        if (r == "worse") required = true;
    }
    CHECK(required);

    // **排在 turn 前面**：先定这一场把局面推到多糟，turn 才是那件事落地的
    // 那一刻；反过来 turn 已经写完了，worse 只能补一个说法，补出来的多半
    // 是 turn 换个说法。
    std::vector<std::string> keys;
    for (auto it = sp.begin(); it != sp.end(); ++it) keys.push_back(it.key());
    const auto at = [&](const std::string& k) {
        return std::find(keys.begin(), keys.end(), k) - keys.begin();
    };
    CHECK(at("worse") < at("turn"));
    CHECK(at("obstacle") < at("worse"));
}

TEST_CASE("最后一句是单独一栏，落库之后接在这一场末尾") {
    // **禁令拦不住就别再加第四道。** 「写完 turn 就停，别再加一段点题」
    // 在提示词、schema 描述、解析守卫里各说了一遍，三道都没拦住——实跑里
    // 一半的章还是在 turn 后面补一段「那一刻，她终于可以告诉自己……」，
    // 而那一段正好落在分集的切线上。把最后一句抬成一个字段，语法里就没有
    // 位置再写下一段了。
    const auto s = changji::stages::chapter_schema(3, 22);
    const auto& scene = s.at("properties").at("scenes").at("items");
    const auto& sp = scene.at("properties");
    REQUIRE(sp.contains("last_line"));
    CHECK(sp.at("last_line").at("maxLength").get<int>() <= 120);  // 一句话的量

    bool required = false;
    for (const auto& r : scene.at("required")) {
        if (r == "last_line") required = true;
    }
    CHECK(required);

    // **排在 paragraphs 后面**：它是收尾，不是开头。
    std::vector<std::string> keys;
    for (auto it = sp.begin(); it != sp.end(); ++it) keys.push_back(it.key());
    const auto at = [&](const std::string& k) {
        return std::find(keys.begin(), keys.end(), k) - keys.begin();
    };
    CHECK(at("paragraphs") < at("last_line"));

    // 落库之后它就是这一场的最后一段，没人分得出它当初是单独一栏
    json paras = json::array();
    for (int i = 0; i < 14; ++i) {
        paras.push_back("第" + std::to_string(i) +
                        "段：她把抹布拧干，水滴落在地板上，溅出细小的一圈。");
    }
    paras.push_back("他停在门口，手扶着门框：“伞我带来了。”");
    paras.push_back("她没抬头，抹布在台面上又抹了一遍：“放那儿吧。”");
    json scenes = json::array();
    scenes.push_back({{"where", "深夜，便利店，冷柜的白光"},
                      {"pov", "林晚"},
                      {"goal", "把伞要回来"},
                      {"obstacle", "他不认这把伞"},
                      {"turn", "伞柄上刻着别人的名字"},
                      {"paragraphs", paras},
                      {"last_line", "她把伞柄转过来，刻着的不是她的名字。"}});
    const auto d = changji::stages::parse_chapter(json{{"scenes", scenes}}.dump());
    REQUIRE(d.scenes.size() == 1);
    CHECK(d.scenes[0].paragraphs.back() == "她把伞柄转过来，刻着的不是她的名字。");
    CHECK(d.text.size() >= 20);
    CHECK(d.text.rfind("她把伞柄转过来，刻着的不是她的名字。") ==
          d.text.size() - std::string("她把伞柄转过来，刻着的不是她的名字。").size());
}

TEST_CASE("换个说法的同一件事，也算撞车") {
    // **2026-09-12 实跑，这两场的 turn 切出来就是相邻两集的钩子**：
    // 一个字都不连着一样，按字面比的守卫不响，而观众看到的是同一件事
    // 演两遍——第二集的信息增量是零。
    // **「换个说法的同一件事」抓不到，而且量过了：** 相邻两字的 Dice
    // 系数，这一对是 0.30，而下面那对真的不同的事是 0.27——分不开。
    // 那个信号在语义里，不在字面里。这一条钉住"我们知道它漏"。
    CHECK_FALSE(changji::stages::scenes_repeat_beat(
        "林悦认出男人是沈嘉诚，并意识到那把伞对她有特殊意义",
        "林悦意识到这把伞对她意义非凡，而对方显然知道这一点"));

    // 一字不差的、互相包含的，照旧要抓住
    CHECK(changji::stages::scenes_repeat_beat("他把伞从天台扔了下去",
                                              "他把伞从天台扔了下去"));
    CHECK(changji::stages::scenes_repeat_beat(
        "伞柄上刻着别人的名字", "她翻过伞柄，伞柄上刻着别人的名字，不是她的"));

    // **真的不同的两件事不能误伤。** 软闸误伤的代价是一次生成变三次。
    CHECK_FALSE(changji::stages::scenes_repeat_beat(
        "林悦抓住沈嘉诚手腕，质问他的选择",
        "陈阿姨递出一个信封，里面装着一张旧照片"));
    CHECK_FALSE(changji::stages::scenes_repeat_beat(
        "她把收银单据折成纸船放进水槽", "他在门口停下，没有回头"));
    // 同一个人做的两件不同的事，也不能算撞
    CHECK_FALSE(changji::stages::scenes_repeat_beat(
        "林悦把伞递给他", "林悦把信收进抽屉锁好"));

    // 太短的不判：一句话不到八个字，重合是巧合
    CHECK_FALSE(changji::stages::scenes_repeat_beat("他走了", "他走了"));
}

TEST_CASE("一段话写两遍就打回") {
    // 复读守卫要一句出现三次才响，而实跑里常见的是同一段二十几个字的话
    // 在一章里出现**两次**——够不着那道闸，却已经是读者能看出来的原地
    // 打转（上一轮 ch03、ch04 各有两段）。
    const auto make = [](bool dup) {
        json paras = json::array();
        for (int i = 0; i < 14; ++i) {
            paras.push_back("第" + std::to_string(i) +
                            "段：她把抹布拧干，水滴落在地板上，溅出细小的一圈。");
        }
        paras.push_back("他停在门口，手扶着门框：“伞我带来了。”");
        paras.push_back("她没抬头，抹布在台面上又抹了一遍：“放那儿吧。”");
        if (dup) {
            paras.push_back("第3段：她把抹布拧干，水滴落在地板上，溅出细小的一圈。");
        }
        json scenes = json::array();
        scenes.push_back({{"where", "深夜，便利店，冷柜的白光"},
                          {"pov", "林晚"},
                          {"goal", "把伞要回来"},
                          {"obstacle", "他不认这把伞"},
                          {"turn", "伞柄上刻着别人的名字"},
                          {"paragraphs", paras},
                          {"last_line", "她把伞柄转过来，刻着的不是她的名字。"}});
        return json{{"scenes", scenes}}.dump();
    };
    CHECK_THROWS_AS(changji::stages::parse_chapter(make(true)),
                    changji::stages::StoryError);
    CHECK_NOTHROW(changji::stages::parse_chapter(make(false)));

    // 软闸：最后一次尝试照收，0 字比「写得一般」差得多
    CHECK_NOTHROW(changji::stages::parse_chapter(make(true), 0, false));
}

TEST_CASE("整章一句对白都没有就打回") {
    // 2026-09-12 实跑四章里有一章通篇零对白（65 段全是叙述）。下一步是把
    // 这段正文改成剧本——正文里没人说话，那一集出来就是默片。和剧本那边
    // 「整集一句台词都没有」是同一道闸。
    const auto make = [](bool spoken) {
        json paras = json::array();
        for (int i = 0; i < 20; ++i) {
            paras.push_back("第" + std::to_string(i) +
                            "段：她把抹布拧干，水滴落在地板上，溅出细小的一圈。");
        }
        if (spoken) {
            // 门槛已经从「整章两处」提到「至少一成的段落」，22 段要三处
            paras.push_back("他停下来，手扶着门框：“伞我带来了。”");
            paras.push_back("她没抬头，抹布在台面上又抹了一遍：“放那儿吧。”");
            paras.push_back("他把伞靠在柜台边上：“那我走了。”");
        }
        json scenes = json::array();
        scenes.push_back({{"where", "深夜，便利店，冷柜的白光"},
                          {"pov", "林晚"},
                          {"goal", "把伞要回来"},
                          {"obstacle", "他不认这把伞"},
                          {"turn", "伞柄上刻着别人的名字"},
                          {"paragraphs", paras}});
        return json{{"scenes", scenes}}.dump();
    };
    // 下限是「至少一成的段落有人说话」：两千字里连几句话都没人说，
    // 那不是独角戏，是对话被整段转述掉了。
    CHECK_THROWS_AS(changji::stages::parse_chapter(make(false)),
                    changji::stages::StoryError);
    CHECK_NOTHROW(changji::stages::parse_chapter(make(true)));

    // **老形状不查。** 粘贴导入和改 schema 之前的草稿不是照着现在这份
    // 提示词写的，拿现在的规矩卡它们只会把打得开的故事变成打不开的。
    std::string plain;
    for (int i = 0; i < 700; ++i) plain += "字";
    CHECK_NOTHROW(changji::stages::parse_chapter(json{{"text", plain}}.dump()));
}

TEST_CASE("说话方式要一路带到正文那一步") {
    Story s = outline_only_story();
    s.characters[0].voice = "短句，从不把话说完，生气时反而更小声";

    const std::string p = changji::stages::build_chapter_prompt(
        s, "ch02", StyleLine::REALISTIC);
    CHECK(p.find("说话：短句，从不把话说完") != std::string::npos);

    // 怕什么也要一路带到正文：只写进人物表的话，就是「小传里有、戏里
    // 没有」，人物行为看着突兀
    s.characters[0].fear = "怕被人看见她根本没打算走";
    const std::string q = changji::stages::build_chapter_prompt(
        s, "ch02", StyleLine::REALISTIC);
    CHECK(q.find("他怕的是：怕被人看见她根本没打算走") != std::string::npos);

    // 弧光也要进来：大纲里填了、人物表里存着，而写正文这一步原来拿不到，
    // 模型写每一章时不知道这个人要往哪儿走
    s.characters[0].arc = "躲着走变成敢直视";
    const std::string r = changji::stages::build_chapter_prompt(
        s, "ch02", StyleLine::REALISTIC);
    CHECK(r.find("他会从躲着走变成敢直视") != std::string::npos);

    // 没写的人不多这一段——粘贴导入的故事和老项目都没有这一栏
    s.characters[0].voice.clear();
    const std::string none = changji::stages::build_chapter_prompt(
        s, "ch02", StyleLine::REALISTIC);
    CHECK(none.find("说话：") == std::string::npos);
}

TEST_CASE("抖出什么要一路带到正文那一步") {
    // 大纲里定了反转，正文那一步不知道的话，那一章照样写成「又见了一面」。
    Story s = outline_only_story();
    s.chapters[1].reveal = "那把伞不是他拿走的，是她母亲塞给他的";

    const std::string p = changji::stages::build_chapter_prompt(
        s, "ch02", StyleLine::REALISTIC);
    CHECK(p.find("这一章要抖出来的是：那把伞不是他拿走的") != std::string::npos);
    // 要它演出来，不是让谁总结一句
    CHECK(p.find("让人看见、听见") != std::string::npos);

    // 没定反转的章不多这一行——粘贴导入的故事和老项目都没有这一栏
    s.chapters[1].reveal.clear();
    const std::string none = changji::stages::build_chapter_prompt(
        s, "ch02", StyleLine::REALISTIC);
    CHECK(none.find("这一章要抖出来的是") == std::string::npos);
}

TEST_CASE("对白用 ASCII 单引号写的，也换成中文双引号") {
    // **2026-09-12 实跑：一个引号的写法吃掉了两道闸。** 整章对白写成
    // '这一次，我们不走回头路了。'，守卫只认弯引号，于是这一章算「一句
    // 对白都没有」，被打回两次；第三次宽松放行，而宽松那次连「两场不能
    // 撞同一件事」也一并跳过——切出来两集的钩子一字不差。
    const auto body = [](const std::string& line) {
        json paras = json::array();
        for (int i = 0; i < 14; ++i) {
            paras.push_back("第" + std::to_string(i) +
                            "段：她把抹布拧干，水滴落在地板上，溅出细小的一圈。");
        }
        paras.push_back(line);
        json scenes = json::array();
        scenes.push_back({{"where", "深夜，便利店，冷柜的白光"},
                          {"pov", "林晚"},
                          {"goal", "把伞要回来"},
                          {"obstacle", "他不认这把伞"},
                          {"turn", "伞柄上刻着别人的名字"},
                          {"paragraphs", paras}});
        return json{{"scenes", scenes}}.dump();
    };

    const auto d = changji::stages::parse_chapter(
        body("他停在门口：'伞我带来了。'她没抬头：'放那儿吧。'"));
    CHECK(d.text.find("“伞我带来了。”") != std::string::npos);
    CHECK(d.text.find("'") == std::string::npos);

    // **落单的撇号不动。** don't、Lin's 里那个不是引号，换了就成了半个
    // 引号挂在句子中间。
    // strict 关掉：这一段本来就没对白，这里要验的只是撇号
    const auto keep = changji::stages::parse_chapter(
        body("他低声说了句什么，听着像 don't。"), 0, false);
    CHECK(keep.text.find("don't") != std::string::npos);
}

TEST_CASE("提示词：没有这一章就抛") {
    const Story s = outline_only_story();
    CHECK_THROWS_AS(
        changji::stages::build_chapter_prompt(s, "ch99", StyleLine::REALISTIC),
        stages::StoryError);
}

TEST_CASE("解析：正文短得离谱的不收") {
    // **实跑时真撞上了**：模型把章标题填进正文字段，四章各写出 1~2 个字，
    // 而这些被静默存了下来——故事看着有四章，分集只切出一集，到写剧本
    // 那一步才发现无米下锅。和剧本那边「整集一句台词都没有」一个道理。
    const std::string tiny = json{{"text", "伞"}}.dump();
    CHECK_THROWS_AS(changji::stages::parse_chapter(tiny, 600), stages::StoryError);
    // 下限给 0 表示不查——拼提示词的单测用得着
    CHECK(changji::stages::parse_chapter(tiny, 0).text == "伞");

    std::string ok;
    for (int i = 0; i < 700; ++i) ok += "字";
    CHECK(changji::text::utf8_len(
              changji::stages::parse_chapter(json{{"text", ok}}.dump(), 600).text) == 700);
}

TEST_CASE("提示词：正文是主要产出，不是顺带的") {
    Story s = outline_only_story();
    const std::string p = changji::stages::build_chapter_prompt(
        s, "ch01", StyleLine::REALISTIC);
    // 钩子那句原来和"写正文"挤在开头同一句里抢注意力
    CHECK(p.find("**主要产出是正文**") != std::string::npos);
    // 篇幅现在是按场给的：每一场多少字。整章那个数模型够不着，
    // 一场一千字它写得到——而下限由 schema 的 minItems/minLength 兜着。
    CHECK(p.find("每一场 ") != std::string::npos);
    CHECK(p.find("不是梗概") != std::string::npos);
}

TEST_CASE("解析：没写出正文就报错，写太多就截断") {
    CHECK_THROWS_AS(changji::stages::parse_chapter(json{{"text", "  "}}.dump()),
                    stages::StoryError);
    CHECK_THROWS_AS(changji::stages::parse_chapter("不是 JSON"),
                    stages::StoryError);

    std::string huge;
    for (int i = 0; i < 30000; ++i) huge += "字";
    const auto d = changji::stages::parse_chapter(
        json{{"text", huge}, {"hook_after", "尾"}}.dump());
    CHECK(changji::text::utf8_len(d.text) == 20000);
}

TEST_CASE("解析：正文里混进模型的解释就打回") {
    // 实跑原样：语法把模型关在 JSON 字符串里，它想纠正自己时那些话落进了
    // 一段 1164 字的正文，字数和复读两道守卫都放过了它
    std::string body;
    for (int i = 0; i < 60; ++i) body += "他推开门，雨声灌了进来。";
    body += "以上内容不符合用户要求的“只输出 JSON”，请忽略此部分内容。";
    CHECK_THROWS_AS(changji::stages::parse_chapter(json{{"text", body}}.dump(), 600),
                    stages::StoryError);
}

TEST_CASE("解析：paragraphs 数组拼成正文，一段一行") {
    const auto d = changji::stages::parse_chapter(
        json{{"paragraphs", json::array({"门铃响了。", "  ", "他抬起头。"})},
             {"hooks", json::array()}}
            .dump(),
        0);
    CHECK(d.text == "门铃响了。\n他抬起头。");
    // 老形状（text 字符串）照样认——粘贴导入和旧草稿走这条
    CHECK(changji::stages::parse_chapter(json{{"text", "伞"}}.dump(), 0).text == "伞");

    // 模型在 JSON 里不敢写 “”，整章对白全用 ‘’：换回中文对白该用的 “”
    const auto q = changji::stages::parse_chapter(
        json{{"paragraphs", json::array({"‘你来了。’她说。", "他没有回答。"})},
             {"hooks", json::array()}}
            .dump(),
        0);
    CHECK(q.text.find("“你来了。”她说。") != std::string::npos);
    // 已经有 “” 的不动——那里的 ‘’ 是套在里面的引号
    CHECK(changji::stages::parse_chapter(json{{"text", "“他说‘走’。”"}}.dump(), 0).text ==
          "“他说‘走’。”");

    // 语法卡了每段最短长度，模型凑数用的引号串（实跑原样）整串删掉；
    // 正常的一对引号不动
    const auto r = changji::stages::parse_chapter(
        json{{"paragraphs", json::array({"她终于决定，是时候面对一切了。”'”””””",
                                         "“走吧。”她说。"})},
             {"hooks", json::array()}}
            .dump(),
        0);
    CHECK(r.text.find("她终于决定，是时候面对一切了。") != std::string::npos);
    CHECK(r.text.find("””") == std::string::npos);
    CHECK(r.text.find("“走吧。”她说。") != std::string::npos);
}

TEST_CASE("并回去：钩子全部重建，说法留着") {
    Story s = outline_only_story();
    // 大纲阶段的钩子挂在 0 上（那时正文是空的）
    REQUIRE(s.chapters[0].hooks.size() == 1);
    CHECK(s.chapters[0].hooks[0].at_char == 0);
    const std::string hook_text = s.chapters[0].hooks[0].text;

    const std::string body =
        "他推门进来，伞还在手里。\n"
        "林晚抬起头。\n"
        "那把伞的骨架断了一根。\n"
        "她认出来了。";
    const Story got = changji::stages::apply_chapter(
        s, "ch01", changji::stages::parse_chapter(
                       good_chapter(body, {{"她认出那把伞", "那把伞的骨架断了一根。"}}).dump()));

    const Chapter& c = got.chapters[0];
    CHECK(c.text == body);

    // **原来挂在 0 上那个不能留**：正文进来之后它就成了「切在章首」
    for (const auto& h : c.hooks) {
        CHECK(h.at_char > 0);
    }

    // 段落边界都登记成候选了
    CHECK(c.hooks.size() >= 3);

    // 大纲那个钩子的说法要留着，并且落在 hook_after 那句之后
    bool found = false;
    for (const auto& h : c.hooks) {
        if (h.text != hook_text) continue;
        found = true;
        const auto chars = changji::text::utf8_chars(c.text);
        REQUIRE(h.at_char > 0);
        REQUIRE(h.at_char <= static_cast<int>(chars.size()));
        CHECK(chars[static_cast<std::size_t>(h.at_char) - 1] == "。");
    }
    CHECK(found);
    CHECK(got.validate().empty());

    // 别的章一个字没动
    CHECK(got.chapters[1].text.empty());
    CHECK(got.chapters[1].summary == s.chapters[1].summary);
}

TEST_CASE("并回去：hook_after 查不到就挂章尾") {
    Story s = outline_only_story();
    const Story got = changji::stages::apply_chapter(
        s, "ch01",
        changji::stages::parse_chapter(
            good_chapter("就这么一段。\n没有第二段。",
                         {{"她认出那把伞", "正文里没有这句"}})
                .dump()));
    const Chapter& c = got.chapters[0];
    bool at_end = false;
    for (const auto& h : c.hooks) {
        if (!h.text.empty() && h.at_char == c.text_len()) at_end = true;
    }
    CHECK(at_end);
    CHECK(got.validate().empty());
}

TEST_CASE("POST /api/story/chapter：写完落库，分集跟着重算") {
    const fs::path root = fresh_project("展开一章");
    ProjectStore store(root);
    Story s = outline_only_story();
    s.episode_duration_s = 60.0;
    s.plan = changji::stages::plan_episodes(s, 60.0);
    store.save_story(s);
    const std::size_t before = s.plan.size();

    // 三千字的一章，配 60 秒（一集 900 字）该切出好几集。
    // 每段都要不一样，理由同上面那处：一模一样的三十段是复读机。
    std::string body;
    for (int i = 0; i < 30; ++i) {
        body += "第" + std::to_string(i) + "段：";
        for (int k = 0; k < 96; ++k) body += "字";
        body += "\n";
    }
    llm::ReplayClient client({good_chapter(body).dump()});
    pipeline::CancelToken tok;

    const auto r = http::post_story_chapter(
        json{{"project", p_str(root)}, {"chapter_id", "ch01"}}, client, tok);
    CHECK(r.status == 200);
    CHECK(r.body.at("chars").get<int>() > 2000);
    // 一集 900 字，但一章的目标是三千——章是故事单元，一章要切出好几集
    CHECK(r.body.at("target_chars").get<int>() ==
          changji::stages::kChapterTargetChars);

    // **这一个是直接落库的**，不像别的几个回草稿
    const Story saved = store.load_story();
    CHECK(saved.written_chapters() == 1);
    // 一章变长了，分集表跟着变多
    CHECK(saved.plan.size() > before);
    CHECK(saved.validate().empty());

    SUBCASE("已经有正文了要显式 overwrite") {
        llm::ReplayClient c2({good_chapter(long_body("重写的正文。")).dump()});
        pipeline::CancelToken t2;
        CHECK_THROWS_AS(
            http::post_story_chapter(
                json{{"project", p_str(root)}, {"chapter_id", "ch01"}}, c2, t2),
            http::ApiError);
        CHECK(c2.calls().empty());

        llm::ReplayClient c3({good_chapter(long_body("重写的正文。")).dump()});
        pipeline::CancelToken t3;
        const auto again = http::post_story_chapter(
            json{{"project", p_str(root)},
                 {"chapter_id", "ch01"},
                 {"overwrite", true}},
            c3, t3);
        CHECK(again.status == 200);
        CHECK(store.load_story().chapters[0].text.rfind("重写的正文。", 0) == 0);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/chapter：没有这一章") {
    const fs::path root = fresh_project("没这章");
    ProjectStore store(root);
    store.save_story(outline_only_story());
    llm::ReplayClient client({good_chapter(long_body("x")).dump()});
    pipeline::CancelToken tok;
    try {
        http::post_story_chapter(
            json{{"project", p_str(root)}, {"chapter_id", "ch99"}}, client, tok);
        FAIL("应该抛");
    } catch (const http::ApiError& e) {
        CHECK(e.status() == 404);
    }
    CHECK(client.calls().empty());
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("展开正文之后，写剧本拿到的是真正文不是梗概") {
    Story s = outline_only_story();
    s.episode_duration_s = 60.0;
    s.plan = changji::stages::plan_episodes(s, 60.0);

    // 没展开正文时，episode_text 返回空，上下文里只能摆梗概
    CHECK(changji::stages::episode_text(s, s.plan[0]).empty());
    const std::string before =
        changji::stages::render_script_context(s, s.plan[0], "");
    CHECK(before.find("他推门进来，伞还在手里。") != std::string::npos);  // 梗概

    // 同 long_body 那段注释：合成的正文要真的不重样，否则复读守卫判废，
    // 而它判得对。
    const std::string body = long_body("这是真正的正文内容。");
    s = changji::stages::apply_chapter(
        s, "ch01", changji::stages::parse_chapter(good_chapter(body).dump()));
    s.plan = changji::stages::plan_episodes(s, 60.0);

    CHECK_FALSE(changji::stages::episode_text(s, s.plan[0]).empty());
    const std::string after =
        changji::stages::render_script_context(s, s.plan[0], "");
    CHECK(after.find("这是真正的正文内容") != std::string::npos);
}

// ---- 批量展开正文 ----

namespace {

/// 等这一轮长跑作业跑完。跑在工作线程上，测试里得等它。
void wait_writer_done() {
    for (int i = 0; i < 600; ++i) {
        if (!changji::pipeline::jobs().running(changji::pipeline::JobKind::Write)) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    FAIL("批量展开跑了十几秒还没结束");
}

}  // namespace

TEST_CASE("POST /api/story/chapters：一口气展开，每写完一章就落库") {
    const fs::path root = fresh_project("批量展开");
    ProjectStore store(root);
    Story s = parse_outline(good_outline().dump(), "深夜便利店", StoryScale::MEDIUM);
    s.episode_duration_s = 60.0;
    s.plan = changji::stages::plan_episodes(s, 60.0);
    store.save_story(s);
    REQUIRE(s.chapters.size() == 2);

    auto client = std::make_shared<llm::ReplayClient>(std::vector<std::string>{
        good_chapter(long_body("第一章的正文。\n他推门进来。\n"),
                     {{"他终于来了", "他推门进来。"}})
            .dump(),
        good_chapter(long_body("第二章的正文。\n她终于开口。\n"),
                     {{"她开口了", "她终于开口。"}})
            .dump(),
    });

    const auto r = http::post_story_chapters(json{{"project", p_str(root)}}, client);
    CHECK(r.status == 200);
    CHECK(r.body.at("started").get<bool>());
    CHECK(r.body.at("chapters").get<int>() == 2);

    wait_writer_done();

    const Story saved = store.load_story();
    CHECK(saved.written_chapters() == 2);
    CHECK(saved.chapters[0].text.find("第一章的正文") != std::string::npos);
    CHECK(saved.chapters[1].text.find("第二章的正文") != std::string::npos);
    CHECK(saved.validate().empty());

    // **第二章的提示词里要带着第一章的结尾。** 每一轮重读盘上的故事就是
    // 为了这个——不重读的话每一章都以为自己接的是空的上一章。
    REQUIRE(client->calls().size() == 2);
    CHECK(client->calls()[0].prompt.find("【上一章是这么结束的】") ==
          std::string::npos);
    CHECK(client->calls()[1].prompt.find("【上一章是这么结束的】") !=
          std::string::npos);
    CHECK(client->calls()[1].prompt.find("他推门进来") != std::string::npos);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/chapters：砸了再要一次，第二次成了就当没事") {
    // 实跑里最常见的砸法是模型把章节标题填进了正文字段，于是正文只有十几
    // 个字。采样带随机种子，再要一次通常就对了——2026-09-11 实跑四章砸了
    // 两章，而这两章的失败彼此无关。
    const fs::path root = fresh_project("砸了重来");
    ProjectStore store(root);
    Story s = parse_outline(good_outline().dump(), "梗概", StoryScale::MEDIUM);
    store.save_story(s);

    auto client = std::make_shared<llm::ReplayClient>(std::vector<std::string>{
        "模型今天想聊点别的",  // 第一章头一次：不是 JSON
        json{{"text", long_body("第一章第二次写出来了。")}}.dump(),
        json{{"text", long_body("第二章写出来了。")}}.dump(),
    });
    http::post_story_chapters(json{{"project", p_str(root)}}, client);
    wait_writer_done();

    const Story saved = store.load_story();
    CHECK(saved.chapters[0].text.find("第一章第二次") != std::string::npos);
    CHECK(saved.chapters[1].text.find("第二章") != std::string::npos);
    CHECK(client->calls().size() == 3);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/chapters：两次都砸了才算砸，别的照写") {
    const fs::path root = fresh_project("写砸一章");
    ProjectStore store(root);
    Story s = parse_outline(good_outline().dump(), "梗概", StoryScale::MEDIUM);
    store.save_story(s);

    auto client = std::make_shared<llm::ReplayClient>(std::vector<std::string>{
        "模型今天想聊点别的",    // 第一章头一次
        "还是想聊点别的",        // 第一章重试
        "第三次也没写",          // 第一章最后一次（软闸关了，但这是硬闸）
        json{{"text", long_body("第二章写出来了。")}}.dump(),
    });
    http::post_story_chapters(json{{"project", p_str(root)}}, client);
    wait_writer_done();

    const Story saved = store.load_story();
    CHECK(saved.chapters[0].text.empty());
    CHECK_FALSE(saved.chapters[1].text.empty());
    // **就多要两次，不是要到成功为止。** 提示词真有毛病时，重试到底只会
    // 把一次失败变成一小时失败。第三次会把软闸关掉（能用但不够好的收下），
    // 而这里三次都不是 JSON——硬闸，收不了。
    CHECK(client->calls().size() == 4);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("POST /api/story/chapters：拦住的几种情况") {
    const fs::path root = fresh_project("批量拦住");
    ProjectStore store(root);
    auto client = std::make_shared<llm::ReplayClient>(
        std::vector<std::string>{json{{"text", "x"}}.dump()});

    SUBCASE("还没有故事") {
        CHECK_THROWS_AS(
            http::post_story_chapters(json{{"project", p_str(root)}}, client),
            http::ApiError);
    }

    SUBCASE("每一章都有正文了") {
        Story s;
        Chapter c;
        c.chapter_id = "ch01";
        c.title = "写过的";
        c.text = "已经有正文了。";
        s.chapters.push_back(c);
        store.save_story(s);
        try {
            http::post_story_chapters(json{{"project", p_str(root)}}, client);
            FAIL("应该抛");
        } catch (const http::ApiError& e) {
            CHECK(e.status() == 400);
        }
        CHECK(client->calls().empty());
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}
