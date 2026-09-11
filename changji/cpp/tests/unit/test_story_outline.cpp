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

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "http/planning.hpp"
#include "http/story_api.hpp"
#include "llm/client.hpp"
#include "models/project.hpp"
#include "models/story.hpp"
#include "stages/bible.hpp"
#include "stages/script_story.hpp"
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
