// 提示词组装的对拍测试。
//
// **这是阶段 5 里唯一算契约的东西。** 出图后端换成什么都无所谓——
// ComfyUI 也好、进程内 sd.cpp 也好，拿到的是同一份提示词。
// 但那串字必须和 Python 逐字节一致，否则同一个项目在两个后端上
// 出的画面不一样，而用户会以为是模型的问题。

#include <doctest/doctest.h>

#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "models/character.hpp"
#include "models/shot.hpp"
#include "stages/prompt_compose.hpp"

using namespace changji;
using json = nlohmann::json;

namespace {

const json& golden() {
    static const json g = [] {
        const std::string path =
            std::string(CHANGJI_GOLDEN_DIR) + "/prompt_compose.json";
        std::ifstream in(path, std::ios::binary);
        REQUIRE_MESSAGE(in.good(), "读不到语料 " << path);
        json j;
        in >> j;
        return j;
    }();
    return g;
}

/// 和语料里那份一模一样的资产库。
models::AssetLibrary make_assets(models::StyleLine line) {
    models::AssetLibrary a;

    models::Character lin;
    lin.char_id = "c_lin_wan";
    lin.name = "林晚";
    lin.appearance.identity = "二十七岁女性，外表冷静";
    lin.appearance.body = "偏瘦，中等身高";
    lin.appearance.face = "黑色长直发，鹅蛋脸，杏眼";
    lin.appearance.attire = "白色衬衫，深色西装裤";
    lin.ref_front = "refs/c_lin_wan_front.png";
    lin.ref_three_quarter = "refs/c_lin_wan_three_quarter.png";
    models::WardrobeVariant rain;
    rain.wardrobe_id = "雨中";
    rain.description = "湿透的白衬衫";
    lin.wardrobe.push_back(rain);
    a.characters["c_lin_wan"] = lin;

    models::Character chen;
    chen.char_id = "c_chen_mo";
    chen.name = "陈默";
    chen.appearance.identity = "三十出头男性";
    chen.appearance.body = "高瘦";
    chen.appearance.face = "短寸黑发，方脸";
    chen.appearance.attire = "黑色风衣";
    a.characters["c_chen_mo"] = chen;

    models::Character plain;
    plain.char_id = "c_plain";
    plain.name = "路人";
    plain.appearance.identity = "中年男性";
    plain.appearance.face = "圆脸";
    plain.appearance.attire = "工装";
    a.characters["c_plain"] = plain;

    models::Location rooftop;
    rooftop.location_id = "loc_rooftop";
    rooftop.name = "夜间天台";
    rooftop.space = "水泥地面，锈蚀护栏";
    rooftop.lighting = "夜间冷调顶光";
    rooftop.palette = "冷蓝";
    rooftop.ref_empty = "refs/loc_rooftop_empty.png";
    a.locations["loc_rooftop"] = rooftop;

    models::Location alley;
    alley.location_id = "loc_alley";
    alley.name = "小巷";
    alley.space = "狭窄砖墙";
    alley.lighting = "路灯昏黄";
    a.locations["loc_alley"] = alley;

    a.style.style_line = line;
    a.style.global_style = "电影感，浅景深，冷色调";
    a.style.negative_prompt = "低质量，模糊";
    a.style.aspect_ratio = "9:16";
    return a;
}

void check_text(const std::string& got, const std::string& want,
                const char* what) {
    if (got != want) {
        std::size_t i = 0;
        while (i < got.size() && i < want.size() && got[i] == want[i]) ++i;
        MESSAGE(what << " 第一处不同在字节 " << i);
        MESSAGE("期望 " << want);
        MESSAGE("实得 " << got);
    }
    CHECK(got == want);
}

}  // namespace

TEST_CASE("提示词组装和 Python 逐字节一致") {
    for (const auto& c : golden().at("cases")) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);

        const auto line = c.at("style_line").get<std::string>() == "anime"
                              ? models::StyleLine::ANIME
                              : models::StyleLine::REALISTIC;
        models::AssetLibrary a = make_assets(line);
        // "全剧风格为空"那条用的是改过的资产库
        if (name == "全剧风格为空") {
            a.style.global_style.clear();
            a.style.negative_prompt.clear();
        }

        const auto shot = c.at("shot").get<models::Shot>();
        const stages::PromptComposer composer(a);

        if (!c.at("ok").get<bool>()) {
            CHECK_THROWS_AS(composer.compose(shot), stages::RenderError);
            continue;
        }

        stages::PromptBundle got;
        REQUIRE_NOTHROW(got = composer.compose(shot));
        check_text(got.positive, c.at("positive").get<std::string>(), "正向");
        check_text(got.negative, c.at("negative").get<std::string>(), "负向");
        CHECK(got.reference_images ==
              c.at("reference_images").get<std::vector<std::string>>());
        check_text(composer.motion_prompt(shot),
                   c.at("motion").get<std::string>(), "运动");
    }
}

TEST_CASE("三张中文标签表一项都不能少") {
    // 表里少一项的话那一层拼出来是空的，而空的景别层意味着模型
    // 自己决定构图——同一集里景别会乱跳，而且看不出是为什么。
    for (const auto& kv : golden().at("shot_size_zh").items()) {
        CAPTURE(kv.key());
        models::ShotSize v{};
        from_json(json(kv.key()), v);
        CHECK(stages::shot_size_zh(v) == kv.value().get<std::string>());
    }
    for (const auto& kv : golden().at("angle_zh").items()) {
        CAPTURE(kv.key());
        models::CameraAngle v{};
        from_json(json(kv.key()), v);
        CHECK(stages::angle_zh(v) == kv.value().get<std::string>());
    }
    for (const auto& kv : golden().at("move_zh").items()) {
        CAPTURE(kv.key());
        models::CameraMove v{};
        from_json(json(kv.key()), v);
        CHECK(stages::move_zh(v) == kv.value().get<std::string>());
    }
}

TEST_CASE("分层顺序不能变") {
    // 提示词里靠前的词权重更高。顺序一变画面重心就跟着变，
    // 同一个角色在不同镜头里会显得不是同一个人。
    //
    // 这一条不比字节，比的是四层出现的**先后**——将来有人重构这个函数时，
    // 逐字节那条用例会报"第一处不同在第 N 字节"，看不出是顺序问题。
    const models::AssetLibrary a = make_assets(models::StyleLine::REALISTIC);
    models::Shot s;
    s.shot_id = "ep01_sh001";
    s.scene_id = "sc01";
    s.first_frame_prompt = "雨夜天台，女子背对镜头";
    s.shot_size = models::ShotSize::MS;
    s.camera_angle = models::CameraAngle::EYE_LEVEL;
    s.location_id = "loc_rooftop";
    models::CharacterInShot in_shot;
    in_shot.char_id = "c_lin_wan";
    s.characters.push_back(in_shot);

    const std::string p = stages::PromptComposer(a).compose(s).positive;
    const std::size_t identity = p.find("二十七岁女性");        // 身份层
    const std::size_t place = p.find("水泥地面");                // 场景层
    const std::size_t camera = p.find("中景");                   // 镜头层
    const std::size_t style = p.find("电影感");                  // 风格层

    REQUIRE(identity != std::string::npos);
    REQUIRE(place != std::string::npos);
    REQUIRE(camera != std::string::npos);
    REQUIRE(style != std::string::npos);
    CHECK(identity < place);
    CHECK(place < camera);
    CHECK(camera < style);
}

TEST_CASE("换装只替换服装那一层") {
    // 换装时把整个外观块换掉的话，同一个角色换件衣服就变了张脸。
    const models::AssetLibrary a = make_assets(models::StyleLine::REALISTIC);
    models::Shot s;
    s.shot_id = "ep01_sh001";
    s.scene_id = "sc01";
    models::CharacterInShot in_shot;
    in_shot.char_id = "c_lin_wan";
    s.characters.push_back(in_shot);

    const stages::PromptComposer composer(a);
    const std::string normal = composer.compose(s).positive;

    s.characters[0].wardrobe_state = "雨中";
    const std::string changed = composer.compose(s).positive;

    // 服装换了
    CHECK(normal.find("白色衬衫，深色西装裤") != std::string::npos);
    CHECK(changed.find("湿透的白衬衫") != std::string::npos);
    CHECK(changed.find("白色衬衫，深色西装裤") == std::string::npos);
    // 脸和身材一个字没动
    CHECK(changed.find("黑色长直发，鹅蛋脸，杏眼") != std::string::npos);
    CHECK(changed.find("偏瘦，中等身高") != std::string::npos);
    CHECK(changed.find("二十七岁女性") != std::string::npos);
}

TEST_CASE("同一个角色在不同镜头里的身份层逐字节相同") {
    // 这是整个一致性方案的根：身份层从资产库读出来，模型碰不到。
    // 两个镜头拿到的必须是同一个字符串，差一个字就是差一张脸。
    const models::AssetLibrary a = make_assets(models::StyleLine::REALISTIC);
    const stages::PromptComposer composer(a);

    const auto make = [](const std::string& id, models::ShotSize size,
                         const std::string& expr) {
        models::Shot s;
        s.shot_id = id;
        s.scene_id = "sc01";
        s.shot_size = size;
        s.first_frame_prompt = "随便什么描述 " + id;
        models::CharacterInShot in_shot;
        in_shot.char_id = "c_lin_wan";
        in_shot.expression = expr;
        s.characters.push_back(in_shot);
        return s;
    };

    const std::string a1 =
        composer.compose(make("sh001", models::ShotSize::CU, "落寞")).positive;
    const std::string a2 =
        composer.compose(make("sh002", models::ShotSize::LS, "克制")).positive;

    // 身份那一段在两个镜头里一模一样
    const std::string identity =
        a.characters.at("c_lin_wan").render_prompt(models::StyleLine::REALISTIC);
    CHECK(a1.find(identity) != std::string::npos);
    CHECK(a2.find(identity) != std::string::npos);
    // 而且是从**开头**开始的——身份层排第一
    CHECK(a1.rfind(identity, 0) == 0);
    CHECK(a2.rfind(identity, 0) == 0);
}

TEST_CASE("两条风格线的分隔符不一样") {
    // 动漫线是 Danbooru 标签串，用英文逗号加空格；写实线是自然语言，
    // 用中文逗号。混用会让标签串里冒出中文标点，
    // 而那对 Danbooru 词表训练出来的模型是噪声。
    models::Shot s;
    s.shot_id = "ep01_sh001";
    s.scene_id = "sc01";
    s.shot_size = models::ShotSize::MS;
    s.camera_angle = models::CameraAngle::EYE_LEVEL;
    // 这一段刻意带中文逗号：它是**内容**，不是分隔符，
    // 动漫线下也要原样留着——改它等于替用户改提示词。
    s.first_frame_prompt = "雨夜天台，女子背对镜头";

    // 别的地方不能有中文标点，否则分不清"分隔符"和"内容自带的逗号"。
    // 资产库里的 global_style 本来就写着"电影感，浅景深"——
    // 那些逗号是内容，不是这条用例要验的东西。
    const auto clean = [](models::StyleLine line) {
        models::AssetLibrary a = make_assets(line);
        a.style.global_style = "cinematic";
        a.style.negative_prompt = "lowres";
        return a;
    };

    const std::string real =
        stages::PromptComposer(clean(models::StyleLine::REALISTIC))
            .compose(s).positive;
    const std::string anime =
        stages::PromptComposer(clean(models::StyleLine::ANIME))
            .compose(s).positive;

    // 写实线两层之间是中文逗号
    CHECK(real == "中景，平视，雨夜天台，女子背对镜头，cinematic");
    // 动漫线是英文逗号加空格，一个中文标点都没有
    CHECK(anime == "中景, 平视, 雨夜天台，女子背对镜头, cinematic");
    // 注意 first_frame_prompt 里那个中文逗号原样留着——
    // 那是内容不是分隔符，改它等于替用户改提示词。
}

TEST_CASE("缺一层不会留下孤零零的分隔符") {
    // 不跳空的话缺一层就多一个"，，"，而那个双逗号会被模型当成停顿信号。
    models::AssetLibrary a = make_assets(models::StyleLine::REALISTIC);
    a.style.global_style.clear();

    models::Shot s;
    s.shot_id = "ep01_sh001";
    s.scene_id = "sc01";
    s.first_frame_prompt.clear();
    s.shot_size = models::ShotSize::MS;
    s.camera_angle = models::CameraAngle::EYE_LEVEL;

    const std::string p = stages::PromptComposer(a).compose(s).positive;
    CHECK(p.find("，，") == std::string::npos);
    CHECK(p.rfind("，") != p.size() - 3);   // 不以分隔符结尾
    CHECK(p.rfind("，", 0) != 0);            // 也不以它开头
}
