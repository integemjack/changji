// 剧本 / 选题 / 预告片阶段的对拍测试。
//
// 语料由 tests/export_script_golden.py 调**真实的 Python 函数**生成。
//
// 这一阶段的难点在文本清洗那三个函数。它们要按 UTF-8 **字符**做事——
// 括号匹配、长度判断、截断都是。按字节做会把汉字劈成半个，
// 而那个半个字节会一路流进提示词和字幕。

#include <doctest/doctest.h>

#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "models/character.hpp"
#include "stages/script.hpp"
#include "util/text.hpp"

using namespace changji;
using json = nlohmann::json;

namespace {

const json& golden() {
    static const json g = [] {
        const std::string path =
            std::string(CHANGJI_GOLDEN_DIR) + "/stage_script.json";
        std::ifstream in(path, std::ios::binary);
        REQUIRE_MESSAGE(in.good(), "读不到语料 " << path);
        json j;
        in >> j;
        return j;
    }();
    return g;
}

models::StyleLine line_from(const std::string& s) {
    return s == "anime" ? models::StyleLine::ANIME : models::StyleLine::REALISTIC;
}

std::vector<std::string> strs(const json& j) {
    return j.get<std::vector<std::string>>();
}

/// 长文本比对失败时报第一处不同，直接比看不出差在哪。
void check_text(const std::string& got, const std::string& want) {
    if (got != want) {
        std::size_t i = 0;
        while (i < got.size() && i < want.size() && got[i] == want[i]) ++i;
        MESSAGE("第一处不同在字节 " << i << "，长度 期望 " << want.size()
                                  << " 实得 " << got.size());
        MESSAGE("期望…" << want.substr(i > 40 ? i - 40 : 0, 110));
        MESSAGE("实得…" << got.substr(i > 40 ? i - 40 : 0, 110));
    }
    CHECK(got == want);
}

}  // namespace

TEST_CASE("削掉整段外面套的括号") {
    for (const auto& c : golden().at("wrappers")) {
        const std::string in = c.at("in").get<std::string>();
        CAPTURE(in);
        CHECK(stages::strip_wrapper(in) == c.at("out").get<std::string>());
    }
}

TEST_CASE("只削套住整段的那一层") {
    // 光数左右个数不够：「（甲说）乙答（丙笑）」左右各两个，数目相等，
    // 但首尾那两个并不是一对，削掉就把中间的括号弄错位了。
    CHECK(stages::strip_wrapper("（甲说）乙答（丙笑）") == "（甲说）乙答（丙笑）");
    CHECK(stages::strip_wrapper("（前）中间（后）") == "（前）中间（后）");
    // 真的套住整段的要削
    CHECK(stages::strip_wrapper("（他犹豫了一下）") == "他犹豫了一下");
    CHECK(stages::strip_wrapper("（（（三层）））") == "三层");
    // 削完是空的就不削
    CHECK(stages::strip_wrapper("（）") == "（）");
}

TEST_CASE("削掉开头的时间码") {
    for (const auto& c : golden().at("timecodes")) {
        const std::string in = c.at("in").get<std::string>();
        CAPTURE(in);
        CHECK(stages::strip_leading_timecode(in) ==
              c.at("out").get<std::string>());
    }
}

TEST_CASE("时间码要既有数字又有单位才削") {
    // 只看数字的话，「（第3次）他又问」开头也会被削掉
    CHECK(stages::strip_leading_timecode("（他犹豫了）他开口") == "（他犹豫了）他开口");
    CHECK(stages::strip_leading_timecode("（第3次）他又问") == "（第3次）他又问");
    CHECK(stages::strip_leading_timecode("[0-3秒] 画面特写：雨水") == "画面特写：雨水");
}

TEST_CASE("说话人归一") {
    for (const auto& c : golden().at("speakers")) {
        const std::string in = c.at("in").get<std::string>();
        CAPTURE(in);
        CHECK(stages::normalize_speaker(in) == c.at("out").get<std::string>());
    }
}

TEST_CASE("none 和旁白不能当成角色名") {
    // 原样当名字用的话，成片字幕上会出现「none：寂静」
    CHECK(stages::normalize_speaker("none").empty());
    CHECK(stages::normalize_speaker("None").empty());
    CHECK(stages::normalize_speaker("旁白").empty());
    CHECK(stages::normalize_speaker("（无）").empty());
    // 正常名字要留着
    CHECK(stages::normalize_speaker("林晚") == "林晚");
    CHECK(stages::normalize_speaker("（林晚）") == "林晚");
}

TEST_CASE("对白字数预算") {
    for (const auto& c : golden().at("budgets")) {
        const double in = c.at("in").get<double>();
        CAPTURE(in);
        CHECK(stages::budget_chars(in) == c.at("out").get<int>());
    }
    // 下限 20 字。时长再短也得能说一句话。
    CHECK(stages::budget_chars(0.0) == 20);
    CHECK(stages::budget_chars(-100.0) == 20);
}

TEST_CASE("三个提示词逐字节钉住：改了必须是有意识地改") {
    // **这条原来叫"和 Python 逐字节一致"**，语料是当年冻下来的 Python
    // 答案，用来证明移植没走样。Python 引擎 2026-09-10 删了之后，这个
    // 用途就没了——而断言还在，实际效果变成"提示词永远改不动"：
    // 想让剧本写得更细，第一步就撞在这儿。
    //
    // 现在它的用途是**快照**：提示词是这套东西的行为核心，改一个字
    // 出来的剧本就不一样，所以不能被顺手改掉。要改就连语料一起改，
    // 让这件事在 diff 里看得见。
    //
    // 2026-09-11 第一次这么改：给剧本加了"动作要拍得出来"那四条
    // （8~11），语料里 22 处跟着更新。premise 和 trailer 一个字没动。

    for (const auto& c : golden().at("prompts")) {
        const std::string kind = c.at("kind").get<std::string>();
        const auto line = line_from(c.at("style_line").get<std::string>());
        CAPTURE(kind);
        CAPTURE(c.at("style_line").get<std::string>());

        std::string got;
        if (kind == "script") {
            CAPTURE(c.at("duration_s").get<double>());
            got = stages::build_script_prompt(
                c.at("premise").get<std::string>(),
                c.at("duration_s").get<double>(), line,
                c.at("previous").get<std::string>(),
                strs(c.at("characters")));
        } else if (kind == "premise") {
            got = stages::build_premise_prompt(
                c.at("keywords").get<std::string>(), line,
                c.at("count").get<int>(), strs(c.at("existing")));
        } else {
            got = stages::build_trailer_prompt(
                c.at("premise").get<std::string>(),
                c.at("duration_s").get<double>(), line,
                c.at("episodes").get<std::string>(),
                strs(c.at("characters")));
        }
        check_text(got, c.at("prompt").get<std::string>());
    }
}

TEST_CASE("时长格式化是银行家舍入") {
    // Python 的 f"{x:.0f}" 和 C 的 %.0f 都是 round-half-to-even。
    // 用四舍五入的话 0.5 会变成 1，提示词里那个数字就和 Python 不一样了。
    const std::string p0 = stages::build_script_prompt(
        "梗概", 0.5, models::StyleLine::REALISTIC);
    CHECK(p0.find("总时长约 0 秒") != std::string::npos);
    const std::string p1 = stages::build_script_prompt(
        "梗概", 1.5, models::StyleLine::REALISTIC);
    CHECK(p1.find("总时长约 2 秒") != std::string::npos);
    const std::string p2 = stages::build_script_prompt(
        "梗概", 2.5, models::StyleLine::REALISTIC);
    CHECK(p2.find("总时长约 2 秒") != std::string::npos);
}

TEST_CASE("超长的前情和剧集要按字符截断") {
    // 按字节截会把最后一个汉字劈成半个，那半个字节直接进提示词。
    const std::string long_prev(5000, 'x');  // 先用 ASCII 确认长度逻辑
    const std::string p = stages::build_script_prompt(
        "梗概", 60.0, models::StyleLine::REALISTIC, long_prev);
    CHECK(p.find(std::string(4000, 'x')) != std::string::npos);
    CHECK(p.find(std::string(4001, 'x')) == std::string::npos);

    SUBCASE("中文的截断点在字符边界上") {
        std::string cn;
        for (int i = 0; i < 5000; ++i) cn += "很";
        const std::string q = stages::build_script_prompt(
            "梗概", 60.0, models::StyleLine::REALISTIC, cn);
        // 截出来的那段必须是 4000 个完整的「很」
        std::string want;
        for (int i = 0; i < 4000; ++i) want += "很";
        CHECK(q.find(want) != std::string::npos);
        CHECK(q.find(want + "很") == std::string::npos);
    }
}

TEST_CASE("解析出的剧本和 Python 一致") {
    for (const auto& c : golden().at("parses")) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);
        const stages::ScriptDraft d =
            stages::parse_script(c.at("raw").get<std::string>());

        CHECK(d.title == c.at("title").get<std::string>());
        CHECK(d.logline == c.at("logline").get<std::string>());

        json beats = json::array();
        for (const stages::Beat& b : d.beats) {
            beats.push_back({{"kind", b.kind},
                             {"speaker", b.speaker},
                             {"text", b.text}});
        }
        if (beats != c.at("beats")) {
            MESSAGE("期望 " << c.at("beats").dump(1));
            MESSAGE("实得 " << beats.dump(1));
        }
        CHECK(beats == c.at("beats"));
        CHECK(d.speakers() == strs(c.at("speakers")));
        CHECK(d.dialogue_chars() == c.at("dialogue_chars").get<std::size_t>());
        check_text(d.render(), c.at("render").get<std::string>());
    }
}

TEST_CASE("解析时的三处兜底") {
    const json& c = golden().at("parses").at(0);
    const stages::ScriptDraft d =
        stages::parse_script(c.at("raw").get<std::string>());

    // 一，说了话却没说是谁说的，降级成动作而不是丢掉。
    //     丢掉的话这句台词就从成片里消失了，那比配错声音还糟。
    bool found_demoted = false;
    for (const auto& b : d.beats) {
        if (b.text == "远处传来汽笛声。") {
            CHECK(b.kind == "action");
            CHECK(b.speaker.empty());
            found_demoted = true;
        }
    }
    CHECK(found_demoted);

    // 二，开头的时间码和外层引号都被削掉了
    for (const auto& b : d.beats) {
        CHECK(b.text.rfind("[0-3秒]", 0) != 0);
        CHECK(b.text.rfind("“", 0) != 0);
    }

    // 三，空文本的那一拍整条丢掉
    for (const auto& b : d.beats) CHECK_FALSE(b.text.empty());
}

TEST_CASE("剧本该报错的都报错") {
    for (const auto& c : golden().at("parse_failures")) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);
        const std::string raw = c.at("raw").get<std::string>();
        if (c.at("raises").get<bool>()) {
            CHECK_THROWS_AS(stages::parse_script(raw), stages::ScriptError);
        } else {
            CHECK_NOTHROW(stages::parse_script(raw));
        }
    }
}

TEST_CASE("解析出的选题和 Python 一致") {
    for (const auto& c : golden().at("premise_parses")) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);
        const auto ideas =
            stages::parse_premises(c.at("raw").get<std::string>());

        json got = json::array();
        for (const auto& i : ideas) {
            got.push_back({{"title", i.title},
                           {"premise", i.premise},
                           {"hook", i.hook}});
        }
        if (got != c.at("ideas")) {
            MESSAGE("期望 " << c.at("ideas").dump(1));
            MESSAGE("实得 " << got.dump(1));
        }
        CHECK(got == c.at("ideas"));
    }
}

TEST_CASE("选题该报错的都报错") {
    for (const auto& c : golden().at("premise_failures")) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);
        const std::string raw = c.at("raw").get<std::string>();
        if (c.at("raises").get<bool>()) {
            CHECK_THROWS_AS(stages::parse_premises(raw), stages::ScriptError);
        } else {
            CHECK_NOTHROW(stages::parse_premises(raw));
        }
    }
}

TEST_CASE("渲染成后面几步认的写法") {
    for (const auto& c : golden().at("renders")) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);
        stages::ScriptDraft d;
        d.title = "t";
        d.logline = "l";
        for (const auto& b : c.at("beats")) {
            d.beats.push_back(stages::Beat{b.at("kind").get<std::string>(),
                                           b.at("speaker").get<std::string>(),
                                           b.at("text").get<std::string>()});
        }
        check_text(d.render(), c.at("render").get<std::string>());
        CHECK(d.speakers() == strs(c.at("speakers")));
        CHECK(d.dialogue_chars() == c.at("dialogue_chars").get<std::size_t>());
    }
}

TEST_CASE("对白字数按字符不按字节") {
    // 中文一个字三字节。用 size() 的话预算判断会偏大三倍，
    // 每一集都会被判成"超出很多"。
    stages::ScriptDraft d;
    d.beats.push_back(stages::Beat{"dialogue", "林晚", "你说过会来的"});
    CHECK(d.dialogue_chars() == 6);
    CHECK(text::utf8_len("你说过会来的") == 6);
}

TEST_CASE("两个 schema 和 Python 一致") {
    CHECK(json(stages::script_schema()) == golden().at("script_schema"));
    CHECK(json(stages::premise_schema()) == golden().at("premise_schema"));
}

// ---- 四段 ----
//
// 2026-09-12 加的。60 秒的集写出 13 秒的剧本，根子是剧本这一层没有承载
// 时长的形状：一个平的 beats 数组，地板写死 4。行业里一集是四拍按秒排的，
// 时长是剧本自己长出来的——所以 schema 长成四段，每段自己的拍数地板。

TEST_CASE("一集按秒切成四段，四段拼起来是整集") {
    const auto acts = stages::act_plan(60.0);
    REQUIRE(acts.size() == 4);
    CHECK(acts[0].key == "opening");
    CHECK(acts[3].key == "cliff");
    CHECK(acts[0].from_s == 0);
    CHECK(acts[3].to_s == 60);
    int sum = 0;
    for (std::size_t i = 0; i < acts.size(); ++i) {
        CAPTURE(acts[i].key);
        CHECK(acts[i].to_s > acts[i].from_s);
        if (i) CHECK(acts[i].from_s == acts[i - 1].to_s);
        CHECK(acts[i].min_beats >= 2);
        CHECK(acts[i].max_beats > acts[i].min_beats);
        sum += acts[i].to_s - acts[i].from_s;
    }
    CHECK(sum == 60);
    // 60 秒：开场 5、推进 28、回报 21、留扣 6
    CHECK(acts[0].to_s == 5);
    CHECK(acts[1].to_s == 33);
    CHECK(acts[2].to_s == 54);
    // 开场不超过 8 秒、留扣不超过 10 秒——三分钟的集也一样，钩子不能拖
    const auto longer = stages::act_plan(180.0);
    CHECK(longer[0].to_s == 8);
    CHECK(longer[3].to_s - longer[3].from_s == 10);
    // 时长小到没法分也不能崩：银行家舍入那几条用例会传 0.5
    const auto tiny = stages::act_plan(0.5);
    REQUIRE(tiny.size() == 4);
    for (const auto& a : tiny) CHECK(a.to_s > a.from_s);
}

TEST_CASE("四段的 schema：四个键按顺序，各自带拍数的地板") {
    const json s = json(stages::script_schema(60.0));
    const auto required = s.at("required").get<std::vector<std::string>>();
    CHECK(required == std::vector<std::string>{"title", "logline", "opening",
                                               "escalation", "payoff", "cliff"});
    const json& esc = s.at("properties").at("escalation").at("properties").at("beats");
    CHECK(esc.at("minItems").get<int>() == stages::act_plan(60.0)[1].min_beats);
    CHECK(esc.at("minItems").get<int>() >= 6);
    CHECK(esc.contains("maxItems"));
    // 每一拍的字数也有地板，空拍凑数在语法层就过不去
    CHECK(esc.at("items").at("properties").at("text").at("minLength").get<int>() >= 2);
    // 平的那份不动：预告片还在用
    CHECK_FALSE(json(stages::script_schema()).at("properties").contains("opening"));
}

TEST_CASE("四段的回包拼成一份平的拍子，渲染时带段头") {
    const std::string raw = R"({"title":"雨","logline":"她等到了",
      "opening":{"beats":[{"kind":"action","speaker":"","text":"天台，雨。"},
                          {"kind":"dialogue","speaker":"林晚","text":"你来了。"}]},
      "escalation":{"beats":[{"kind":"dialogue","speaker":"陈默","text":"我不该来。"}]},
      "payoff":{"beats":[{"kind":"action","speaker":"","text":"她把伞递过去。"}]},
      "cliff":{"beats":[{"kind":"dialogue","speaker":"陈默","text":"伞不是我的。"}]}})";
    const stages::ScriptDraft d = stages::parse_script(raw, 60.0);
    REQUIRE(d.acts.size() == 4);
    CHECK(d.beats.size() == 5);
    CHECK(d.acts[0].beats.size() == 2);
    CHECK(d.acts[0].from_s == 0);
    CHECK(d.acts[0].to_s == 5);
    CHECK(d.beats[1].speaker == "林晚");
    CHECK(d.beats[4].text == "伞不是我的。");
    const std::string text = d.render();
    CHECK(text.rfind("【开场钩子 0–5 秒】\n天台，雨。\n林晚：你来了。\n【冲突推进 5–33 秒】", 0) == 0);
    CHECK(stages::is_act_header("【开场钩子 0–5 秒】"));
    // 段头去掉之后就是原来那份
    CHECK(stages::strip_act_headers(text) ==
          "天台，雨。\n林晚：你来了。\n陈默：我不该来。\n她把伞递过去。\n陈默：伞不是我的。");
    // 不带时长解析：段头没有秒数，但段还在
    const stages::ScriptDraft d0 = stages::parse_script(raw);
    CHECK(d0.acts.size() == 4);
    CHECK(d0.render().rfind("【开场钩子】\n", 0) == 0);
}

TEST_CASE("平的回包照旧，没有段头") {
    const std::string raw =
        R"({"title":"雨","logline":"x","beats":[{"kind":"dialogue","speaker":"林晚","text":"你来了。"}]})";
    const stages::ScriptDraft d = stages::parse_script(raw, 60.0);
    CHECK(d.acts.empty());
    CHECK(d.render() == "林晚：你来了。");
}

TEST_CASE("段头识别不误伤正常的拍子") {
    CHECK(stages::is_act_header("【集尾留扣 54–60 秒】"));
    CHECK(stages::is_act_header("【情绪回报】"));
    std::string label;
    int from = -1, to = -1;
    CHECK(stages::parse_act_header("【冲突推进 5-33 秒】", &label, &from, &to));
    CHECK(label == "冲突推进");
    CHECK(from == 5);
    CHECK(to == 33);
    CHECK_FALSE(stages::is_act_header("【字幕】三年后"));
    CHECK_FALSE(stages::is_act_header("【倒计时 10 秒】"));
    CHECK_FALSE(stages::is_act_header("【三年后的一天】"));
    CHECK_FALSE(stages::is_act_header("林晚：走。"));
}

TEST_CASE("提示词里写明四段各占几秒、至少几拍") {
    const std::string p =
        stages::build_script_prompt("梗概", 60.0, models::StyleLine::REALISTIC);
    CHECK(p.find("开场钩子（0–5 秒）") != std::string::npos);
    CHECK(p.find("集尾留扣（54–60 秒）") != std::string::npos);
    const auto acts = stages::act_plan(60.0);
    CHECK(p.find("至少 " + std::to_string(acts[1].min_beats) + " 拍") !=
          std::string::npos);
}

TEST_CASE("动作行开头的机位标签削掉") {
    // 实跑里模型写的那一行。不削的话渲染出来和一句台词一模一样，
    // 下游会当成一个叫「镜头特写」的角色在说话。
    CHECK(stages::strip_camera_prefix("镜头特写：病历单上的日期是三年前。") ==
          "病历单上的日期是三年前。");
    CHECK(stages::strip_camera_prefix("特写:她的手") == "她的手");
    CHECK(stages::strip_camera_prefix("全景：雨中的街道") == "雨中的街道");

    // 正文里本来就有的冒号不动
    CHECK(stages::strip_camera_prefix("牌子上写着：营业中") == "牌子上写着：营业中");
    CHECK(stages::strip_camera_prefix("林浩把箱子放下。") == "林浩把箱子放下。");
    // 冒号前太长的不是标签
    CHECK(stages::strip_camera_prefix("他盯着那块画面很久才说：走吧") ==
          "他盯着那块画面很久才说：走吧");
    // 只有标签没内容时留着，削成空串这一拍会被丢掉
    CHECK(stages::strip_camera_prefix("特写：") == "特写：");

    SUBCASE("只削动作行，台词不动") {
        const std::string raw =
            R"({"title":"x","logline":"y","beats":[
                {"kind":"action","speaker":"","text":"镜头特写：那张纸"},
                {"kind":"dialogue","speaker":"林浩","text":"你听我说：别走"}]})";
        const stages::ScriptDraft d = stages::parse_script(raw);
        CHECK(d.beats[0].text == "那张纸");
        CHECK(d.beats[1].text == "你听我说：别走");
    }
}
