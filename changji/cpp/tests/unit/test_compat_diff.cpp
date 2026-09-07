// 对拍用的深比较的测试。
//
// **这个比较器错了，整个对拍就是假的。** 比得太松会放过真差异——
// 而对拍是删 Python 之前唯一的验收手段；比得太严则会淹在噪音里，
// 人看两屏无关的差异之后就不看了，效果和没有一样。

#include <doctest/doctest.h>

#include <string>

#include "../compat/diff.hpp"

using namespace changji;
using json = nlohmann::json;

TEST_CASE("对象的键顺序不算差异") {
    // 契约标准是结构兼容，key 顺序不管。nlohmann 的 json 本来就按键排序，
    // 但这一条要写下来——将来有人为了别的目的换成 ordered_json 时，
    // 这条用例会拦住他。
    const json a = {{"b", 2}, {"a", 1}};
    const json b = {{"a", 1}, {"b", 2}};
    CHECK(compat::compare(a, b).empty());
}

TEST_CASE("数组的顺序是算数的") {
    // 镜头的先后就是成片的先后。把数组当集合比的话，顺序反了也算通过。
    const json a = json::array({1, 2, 3});
    const json b = json::array({3, 2, 1});
    const auto diffs = compat::compare(a, b);
    CHECK(diffs.size() == 2);
    CHECK(diffs[0].path == "/0");
}

TEST_CASE("整数和浮点是同一个值") {
    // JSON 不区分整浮点，而 Python 那边一个字段是 int 还是 float
    // 取决于它是怎么算出来的——5 和 5.0 报成差异的话，
    // 每一份语料都会有几十条假差异。
    CHECK(compat::compare(json(5), json(5.0)).empty());
    CHECK(compat::compare(json(0), json(-0.0)).empty());

    SUBCASE("但数字和字符串不是") {
        CHECK_FALSE(compat::compare(json(5), json("5")).empty());
    }

    SUBCASE("浮点留一点容差") {
        // 两边算法一样但求值顺序可能不同，完全相等的要求会在
        // 第十几位小数上炸。
        CHECK(compat::compare(json(1.0), json(1.0 + 1e-15)).empty());
        CHECK_FALSE(compat::compare(json(1.0), json(1.01)).empty());
    }
}

TEST_CASE("null 和字段不存在是两回事") {
    // Python 的 opt_str 会吐 null，少一个键是真的差异——
    // 前端可能按 in 判断字段存不存在。
    const json with_null = {{"a", nullptr}};
    const json missing = json::object();
    const auto diffs = compat::compare(with_null, missing);
    REQUIRE(diffs.size() == 1);
    CHECK(diffs[0].path == "/a");
    CHECK(diffs[0].detail.find("少了") != std::string::npos);

    SUBCASE("两边都是 null 就一致") {
        CHECK(compat::compare(with_null, json{{"a", nullptr}}).empty());
    }
}

TEST_CASE("多出来的键也是差异") {
    // 只遍历期望那一边的话，多出来的键查不出来。而多一个键同样破契约。
    const json a = {{"x", 1}};
    const json b = {{"x", 1}, {"y", 2}};
    const auto diffs = compat::compare(a, b);
    REQUIRE(diffs.size() == 1);
    CHECK(diffs[0].path == "/y");
    CHECK(diffs[0].detail.find("多了") != std::string::npos);
}

TEST_CASE("不在第一处差异就停") {
    // 对拍要起两个后端，一轮下来是分钟级。一次跑完看到所有问题，
    // 比修一个跑一次快得多。
    const json a = {{"a", 1}, {"b", 2}, {"c", 3}};
    const json b = {{"a", 9}, {"b", 9}, {"c", 9}};
    CHECK(compat::compare(a, b).size() == 3);
}

TEST_CASE("数组长度不同时公共部分照样比") {
    // 只报一句"长度不同"等于让人自己去找。差异往往集中在头几个元素上。
    const json a = json::array({1, 2, 3});
    const json b = json::array({9, 2});
    const auto diffs = compat::compare(a, b);
    CHECK(diffs.size() == 2);   // 长度 + 第 0 个
    bool len = false, first = false;
    for (const auto& d : diffs) {
        if (d.detail.find("长度不同") != std::string::npos) len = true;
        if (d.path == "/0") first = true;
    }
    CHECK(len);
    CHECK(first);
}

TEST_CASE("嵌套结构的路径要指得准") {
    // 报"某处不一样"没用。一份项目文件几百行，路径不准等于没报。
    const json a = {{"episodes", json::array({
        {{"shots", json::array({{{"duration_s", 5.0}}})}}})}};
    const json b = {{"episodes", json::array({
        {{"shots", json::array({{{"duration_s", 8.0}}})}}})}};
    const auto diffs = compat::compare(a, b);
    REQUIRE(diffs.size() == 1);
    CHECK(diffs[0].path == "/episodes/0/shots/0/duration_s");
}

TEST_CASE("键里带斜杠时路径要转义") {
    // 项目里的相对路径就带斜杠（shots/draft/a.mp4）。不转义的话
    // 路径会被切成两段，报出来的位置是错的。
    const json a = {{"shots/draft", 1}};
    const json b = {{"shots/draft", 2}};
    const auto diffs = compat::compare(a, b);
    REQUIRE(diffs.size() == 1);
    CHECK(diffs[0].path == "/shots~1draft");
}

TEST_CASE("路径匹配：* 吃一段，** 吃任意多段") {
    // 模式写错了不会报错，只会静默地少比或者多比——
    // 少比的那部分就再也没被对拍过了。
    CHECK(compat::path_matches("/a/b", "/a/b"));
    CHECK(compat::path_matches("/a/b", "/a/*"));
    CHECK(compat::path_matches("/a/b", "/*/b"));
    CHECK_FALSE(compat::path_matches("/a/b/c", "/a/*"));
    CHECK_FALSE(compat::path_matches("/a", "/a/*"));

    CHECK(compat::path_matches("/a/b/c/d", "/a/**"));
    CHECK(compat::path_matches("/a", "/a/**"));      // ** 可以吃 0 段
    CHECK(compat::path_matches("/x/y/mtime", "/**/mtime"));
    CHECK(compat::path_matches("/mtime", "/**/mtime"));
    CHECK_FALSE(compat::path_matches("/x/mtimes", "/**/mtime"));

    SUBCASE("根路径") {
        CHECK(compat::path_matches("", ""));
        CHECK(compat::path_matches("", "/**"));
    }
}

TEST_CASE("忽略规则按路径生效，而且整棵子树都跳过") {
    const json a = {{"data", {{"x", 1}}}, {"mtime", 100}};
    const json b = {{"data", {{"x", 2}}}, {"mtime", 999}};

    compat::CompareOptions opts;
    opts.ignore = {{"/mtime", "文件时间戳，两边跑的时刻不同"}};
    const auto diffs = compat::compare(a, b, opts);
    REQUIRE(diffs.size() == 1);
    CHECK(diffs[0].path == "/data/x");   // 只忽略了 mtime

    SUBCASE("忽略一个对象就连它下面全跳过") {
        compat::CompareOptions o2;
        o2.ignore = {{"/data", "整块都是本机相关的"}};
        const auto d2 = compat::compare(a, b, o2);
        REQUIRE(d2.size() == 1);
        CHECK(d2[0].path == "/mtime");
    }

    SUBCASE("通配能覆盖数组里每一项") {
        const json x = {{"files", json::array({{{"mtime", 1}}, {{"mtime", 2}}})}};
        const json y = {{"files", json::array({{{"mtime", 9}}, {{"mtime", 8}}})}};
        compat::CompareOptions o3;
        o3.ignore = {{"/files/*/mtime", "文件时间戳"}};
        CHECK(compat::compare(x, y, o3).empty());
    }
}

TEST_CASE("报告里带路径和值，而且有上限") {
    const json a = {{"a", 1}};
    const json b = {{"a", 2}};
    const std::string s = compat::format(compat::compare(a, b));
    CAPTURE(s);
    CHECK(s.find("/a") != std::string::npos);
    CHECK(s.find("1") != std::string::npos);
    CHECK(s.find("2") != std::string::npos);

    SUBCASE("一致时说一致") {
        CHECK(compat::format({}) == "一致");
    }

    SUBCASE("差异太多时截断，但说清还有多少") {
        // 两千条差异全打出来的话，终端里翻不到头，人就不看了。
        std::vector<compat::Difference> many;
        for (int i = 0; i < 100; ++i) {
            many.push_back({"/x" + std::to_string(i), "不一样"});
        }
        const std::string t = compat::format(many, 5);
        CHECK(t.find("还有 95 处") != std::string::npos);
    }
}

TEST_CASE("长值在报告里要截断") {
    // 一个字段是三千字的提示词时，整段打出来会把别的差异挤出屏幕。
    const json a = {{"prompt", std::string(500, 'x')}};
    const json b = {{"prompt", std::string(500, 'y')}};
    const std::string s = compat::format(compat::compare(a, b));
    CHECK(s.size() < 500);
    CHECK(s.find("…") != std::string::npos);
}
