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
#include "stages/story_outline.hpp"
#include "util/paths.hpp"

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
