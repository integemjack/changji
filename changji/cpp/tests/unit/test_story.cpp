// 故事层与分集算法。
//
// 这里钉住的是「集数是算出来的」这条规矩的具体行为：同一个故事配不同的
// 每集时长，切出来的集数必须跟着变，而切点只能落在钩子或章界上。
//
// 正文一律用汉字构造（zh(n) 给 n 个字），不用 ASCII：切分全程按 UTF-8
// **字符**算，用 ASCII 的话字符数和字节数相等，这个测试就测不到那个区别了，
// 而那正是最容易写错、错了又最难查的地方（切线落在汉字中间，截出来的是
// 非法 UTF-8，最后表现成离得很远的故障）。

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "models/project.hpp"
#include "models/story.hpp"
#include "stages/story_plan.hpp"
#include "util/paths.hpp"

using namespace changji::models;
using changji::stages::plan_episodes;
using changji::stages::prose_budget_chars;

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

/// n 个汉字。每个 1 字符 3 字节。
std::string zh(int n) {
    std::string s;
    s.reserve(static_cast<std::size_t>(n) * 3);
    for (int i = 0; i < n; ++i) s += "字";
    return s;
}

Chapter mk(const std::string& id, const std::string& title, int len) {
    Chapter c;
    c.chapter_id = id;
    c.title = title;
    c.text = zh(len);
    return c;
}

Chapter outline_only(const std::string& id, const std::string& title) {
    Chapter c;
    c.chapter_id = id;
    c.title = title;
    c.summary = "大纲阶段只有梗概，还没展开正文";
    return c;
}

Hook hook(int at, const std::string& text) {
    Hook h;
    h.at_char = at;
    h.text = text;
    return h;
}

}  // namespace

TEST_CASE("空故事是空的，老项目靠它走老路径") {
    Story s;
    CHECK(s.empty());
    CHECK(s.written_chapters() == 0);

    s.chapters.push_back(outline_only("ch01", "雨夜重逢"));
    CHECK_FALSE(s.empty());
    // 只有大纲不算写过正文
    CHECK(s.written_chapters() == 0);

    s.chapters[0].text = zh(100);
    CHECK(s.written_chapters() == 1);
}

TEST_CASE("Story 往返序列化，中文原样进出") {
    Story s;
    s.premise = "深夜便利店，前任推门进来，手里拿着五年前她送的那把伞。";
    s.scale = StoryScale::LONG;
    s.source = StorySource::PASTED;
    s.logline = "一把伞牵出五年前的事";
    s.genre = "都市情感";
    s.tone = "克制";
    s.episode_duration_s = 90.0;

    StoryCharacter c;
    c.name = "林晚";
    c.identity = "便利店夜班店员";
    c.want = "把那把伞还回去，然后彻底了断";
    c.arc = "从躲着走到敢直视";
    s.characters.push_back(c);

    Relation r;
    r.a = "林晚";
    r.b = "林晚";
    r.kind = "自我";
    r.tension = "不肯承认还在意";
    s.relations.push_back(r);

    StoryLocation loc;
    loc.name = "便利店";
    loc.what = "临街的 24 小时便利店";
    loc.when = "深夜，冷白顶光";
    s.locations.push_back(loc);

    Chapter ch = mk("ch01", "雨夜重逢", 12);
    ch.summary = "他推门进来";
    ch.hooks.push_back(hook(6, "她认出那把伞"));
    ch.characters.push_back("林晚");
    ch.locations.push_back("便利店");
    s.chapters.push_back(ch);

    const Story back = json(s).get<Story>();

    CHECK(back.premise == s.premise);
    CHECK(back.scale == StoryScale::LONG);
    CHECK(back.source == StorySource::PASTED);
    CHECK(back.logline == s.logline);
    CHECK(back.episode_duration_s == doctest::Approx(90.0));
    REQUIRE(back.characters.size() == 1);
    CHECK(back.characters[0].name == "林晚");
    CHECK(back.characters[0].want == c.want);
    REQUIRE(back.relations.size() == 1);
    CHECK(back.relations[0].tension == r.tension);
    REQUIRE(back.locations.size() == 1);
    CHECK(back.locations[0].when == loc.when);
    REQUIRE(back.chapters.size() == 1);
    CHECK(back.chapters[0].title == "雨夜重逢");
    CHECK(back.chapters[0].text_len() == 12);
    REQUIRE(back.chapters[0].hooks.size() == 1);
    CHECK(back.chapters[0].hooks[0].at_char == 6);
    CHECK(back.chapters[0].hooks[0].text == "她认出那把伞");

    // 老版本写的文件里没有新字段，读的时候要能退到默认值而不是抛。
    json partial = json{{"premise", "只有梗概"}};
    const Story old = partial.get<Story>();
    CHECK(old.premise == "只有梗概");
    CHECK(old.scale == StoryScale::MEDIUM);
    CHECK(old.chapters.empty());
}

TEST_CASE("validate 拦住会让切分出事的那几种坏数据") {
    SUBCASE("章节 id 形状不对") {
        Story s;
        s.chapters.push_back(mk("第一章", "雨夜重逢", 10));
        const auto errs = s.validate();
        REQUIRE(errs.size() == 1);
        CHECK(errs[0].find("ch 加数字") != std::string::npos);
    }

    SUBCASE("章节 id 重复") {
        Story s;
        s.chapters.push_back(mk("ch01", "甲", 10));
        s.chapters.push_back(mk("ch01", "乙", 10));
        const auto errs = s.validate();
        REQUIRE(errs.size() == 1);
        CHECK(errs[0].find("重复") != std::string::npos);
    }

    SUBCASE("钩子越界——它会被当成切点，越界就切出空的一集") {
        Story s;
        Chapter c = mk("ch01", "雨夜重逢", 10);
        c.hooks.push_back(hook(50, "越界了"));
        s.chapters.push_back(c);
        const auto errs = s.validate();
        REQUIRE(errs.size() == 1);
        CHECK(errs[0].find("超出正文范围") != std::string::npos);
    }

    SUBCASE("关系指向不存在的人物") {
        Story s;
        StoryCharacter c;
        c.name = "林晚";
        s.characters.push_back(c);
        Relation r;
        r.a = "林晚";
        r.b = "陈默";
        s.relations.push_back(r);
        const auto errs = s.validate();
        REQUIRE(errs.size() == 1);
        CHECK(errs[0].find("陈默") != std::string::npos);
    }

    SUBCASE("分集表指向不存在的章") {
        Story s;
        s.chapters.push_back(mk("ch01", "甲", 10));
        EpisodePlan p;
        p.episode_id = "ep01";
        p.from_chapter = "ch01";
        p.to_chapter = "ch09";
        s.plan.push_back(p);
        const auto errs = s.validate();
        REQUIRE(errs.size() == 1);
        CHECK(errs[0].find("ch09") != std::string::npos);
    }

    SUBCASE("好数据不报错") {
        Story s;
        s.premise = "深夜便利店";
        Chapter c = mk("ch01", "雨夜重逢", 10);
        c.hooks.push_back(hook(10, "章尾也是合法位置"));
        s.chapters.push_back(c);
        CHECK(s.validate().empty());
    }
}

TEST_CASE("每集容量按时长算") {
    CHECK(prose_budget_chars(60.0) == 900);
    CHECK(prose_budget_chars(30.0) == 450);
    CHECK(prose_budget_chars(0.0) == 0);
    CHECK(prose_budget_chars(-1.0) == 0);
}

TEST_CASE("没展开正文时一章一集") {
    Story s;
    s.chapters.push_back(outline_only("ch01", "雨夜重逢"));
    s.chapters.push_back(outline_only("ch02", "五年前那把伞"));
    s.chapters.push_back(outline_only("ch03", "便利店打烊"));

    const auto plan = plan_episodes(s, 60.0);
    REQUIRE(plan.size() == 3);
    CHECK(plan[0].episode_id == "ep01");
    CHECK(plan[0].title == "雨夜重逢");
    CHECK(plan[0].from_chapter == "ch01");
    CHECK(plan[2].episode_id == "ep03");
    CHECK(plan[2].to_chapter == "ch03");
    // 一章一集时不加上/下后缀
    CHECK(plan[1].title == "五年前那把伞");
}

TEST_CASE("切点落在钩子上，不按字数硬切") {
    // 容量 900。钩子在 850 和 1400，正文 2000 字。
    Story s;
    Chapter c = mk("ch01", "雨夜重逢", 2000);
    c.hooks.push_back(hook(850, "她认出那把伞"));
    c.hooks.push_back(hook(1400, "他没有回头"));
    s.chapters.push_back(c);

    const auto plan = plan_episodes(s, 60.0);
    REQUIRE(plan.size() == 2);

    // 第一刀切在 850 那个钩子上——离理想位置 900 最近的候选
    CHECK(plan[0].from_char == 0);
    CHECK(plan[0].to_char == 850);
    CHECK(plan[0].hook == "她认出那把伞");

    // 剩下 1150 字不到 1.3 个容量，并成一集而不是再切一刀
    CHECK(plan[1].from_char == 850);
    CHECK(plan[1].to_char == 2000);
    CHECK(plan[1].hook.empty());

    // 同一章切成两集，标题要分得开
    CHECK(plan[0].title == "雨夜重逢（上）");
    CHECK(plan[1].title == "雨夜重逢（下）");
}

TEST_CASE("尾巴不单切") {
    // 1000 字、容量 900：切的话会留一个 100 字的尾巴，宁可并成一集
    Story s;
    s.chapters.push_back(mk("ch01", "雨夜重逢", 1000));

    const auto plan = plan_episodes(s, 60.0);
    REQUIRE(plan.size() == 1);
    CHECK(plan[0].to_char == 1000);
    CHECK(plan[0].title == "雨夜重逢");
}

TEST_CASE("短章合并成一集，跨章") {
    Story s;
    s.chapters.push_back(mk("ch01", "甲", 300));
    s.chapters.push_back(mk("ch02", "乙", 300));

    const auto plan = plan_episodes(s, 60.0);
    REQUIRE(plan.size() == 1);
    CHECK(plan[0].from_chapter == "ch01");
    CHECK(plan[0].from_char == 0);
    CHECK(plan[0].to_chapter == "ch02");
    CHECK(plan[0].to_char == 300);
}

TEST_CASE("一章切三集用上中下") {
    Story s;
    Chapter c = mk("ch01", "雨夜重逢", 3000);
    c.hooks.push_back(hook(900, "钩子一"));
    c.hooks.push_back(hook(1800, "钩子二"));
    s.chapters.push_back(c);

    const auto plan = plan_episodes(s, 60.0);
    REQUIRE(plan.size() == 3);
    CHECK(plan[0].title == "雨夜重逢（上）");
    CHECK(plan[1].title == "雨夜重逢（中）");
    CHECK(plan[2].title == "雨夜重逢（下）");
    CHECK(plan[0].hook == "钩子一");
    CHECK(plan[1].hook == "钩子二");
}

TEST_CASE("每集时长变了，集数跟着变——集数是算出来的") {
    Story s;
    Chapter c = mk("ch01", "雨夜重逢", 3000);
    for (int at = 300; at < 3000; at += 300) {
        c.hooks.push_back(hook(at, "钩子" + std::to_string(at)));
    }
    s.chapters.push_back(c);

    const auto long_eps = plan_episodes(s, 120.0); // 容量 1800
    const auto short_eps = plan_episodes(s, 30.0); // 容量 450

    CHECK(long_eps.size() < short_eps.size());
    // 每集时长要落到分集表上，后面几步照着它排镜头
    CHECK(long_eps[0].target_duration_s == doctest::Approx(120.0));
    CHECK(short_eps[0].target_duration_s == doctest::Approx(30.0));

    // 切点全都落在钩子上（300 的整数倍），一个都不许落在别处
    for (const auto& p : short_eps) {
        CHECK(p.to_char % 300 == 0);
    }
}

TEST_CASE("有正文和没正文的章交替时顺序不乱") {
    Story s;
    s.chapters.push_back(outline_only("ch01", "甲"));
    s.chapters.push_back(mk("ch02", "乙", 1000));
    s.chapters.push_back(outline_only("ch03", "丙"));

    const auto plan = plan_episodes(s, 60.0);
    REQUIRE(plan.size() == 3);
    CHECK(plan[0].from_chapter == "ch01");
    CHECK(plan[1].from_chapter == "ch02");
    CHECK(plan[2].from_chapter == "ch03");
    CHECK(plan[0].episode_id == "ep01");
    CHECK(plan[2].episode_id == "ep03");
}

TEST_CASE("时长没给就退回故事上存的那个") {
    Story s;
    s.episode_duration_s = 30.0;
    s.chapters.push_back(mk("ch01", "雨夜重逢", 1000));

    const auto plan = plan_episodes(s, 0.0);
    REQUIRE(!plan.empty());
    CHECK(plan[0].target_duration_s == doctest::Approx(30.0));
}

TEST_CASE("切出来的分集表自己能过校验") {
    Story s;
    Chapter c = mk("ch01", "雨夜重逢", 3000);
    c.hooks.push_back(hook(900, "钩子一"));
    c.hooks.push_back(hook(1800, "钩子二"));
    s.chapters.push_back(c);
    s.plan = plan_episodes(s, 60.0);
    CHECK(s.validate().empty());
}

TEST_CASE("story.json 读写：不存在时是空的，存完读回来一样") {
    const fs::path root =
        fs::temp_directory_path() / changji::paths::from_utf8("changji_故事_读写");
    std::error_code ec;
    fs::remove_all(root, ec);

    ProjectStore store = ProjectStore::create(root, "gushi", "故事测试项目");

    // 老项目没有这个文件，要读出一个空的而不是抛
    CHECK(store.load_story().empty());

    Story s;
    s.premise = "深夜便利店，前任推门进来。";
    s.scale = StoryScale::SHORT;
    Chapter c = mk("ch01", "雨夜重逢", 20);
    c.hooks.push_back(hook(10, "她认出那把伞"));
    s.chapters.push_back(c);
    s.plan = plan_episodes(s, 60.0);
    store.save_story(s);

    const Story back = store.load_story();
    CHECK_FALSE(back.empty());
    CHECK(back.premise == s.premise);
    CHECK(back.scale == StoryScale::SHORT);
    REQUIRE(back.chapters.size() == 1);
    CHECK(back.chapters[0].hooks[0].text == "她认出那把伞");
    CHECK(back.plan.size() == s.plan.size());

    // 故事单独一个文件，不跟 project.json 搅在一起——正文十几万字，
    // 而 project.json 每改一次分镜状态就要原子重写一遍。
    CHECK(fs::is_regular_file(store.paths().story_file()));
    const Project p = store.load_project();
    CHECK(p.episodes.empty());

    fs::remove_all(root, ec);
}
