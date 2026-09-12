// 「这个能力派给哪台」的三层过滤。
//
// 挑错了都不报错：挑了台干不了的是那一镜失败，该挑的没挑是那台一直闲着
// 而界面上看不出为什么。所以这一段要能反复撞。
//
// 最要紧的一条是最后那组：**一台都派不出去时，得说清是哪一层拦的**。
// 「连不上」「你自己关的」「它干不了」这三种，用户要做的事完全不同——
// 等一等／去表上打开／去装模型。

#include <doctest/doctest.h>

#include "infer/node_pick.hpp"

using namespace changji::infer;

namespace {

NodeState node(std::string url, bool online, std::set<Capability> able,
               std::set<Capability> off = {}, bool busy = false) {
    NodeState n;
    n.url = std::move(url);
    n.name = n.url;
    n.online = online;
    n.able = std::move(able);
    n.off = std::move(off);
    n.busy = busy;
    return n;
}

}  // namespace

TEST_CASE("三层：在线、没被关、自己说能干") {
    const std::vector<NodeState> nodes = {
        node("a", true, {Capability::Frame, Capability::Video}),
        node("b", false, {Capability::Frame}),                      // 连不上
        node("c", true, {Capability::Frame}, {Capability::Frame}),  // 你关的
        node("d", true, {Capability::Llm}),                         // 干不了
    };
    const auto cands = candidates_for(nodes, Capability::Frame);
    REQUIRE(cands.size() == 1);
    CHECK(cands.front()->url == "a");
}

TEST_CASE("空闲的优先") {
    const std::vector<NodeState> nodes = {
        node("busy", true, {Capability::Video}, {}, /*busy=*/true),
        node("idle", true, {Capability::Video}),
    };
    CHECK(pick_for(nodes, Capability::Video) == "idle");
}

TEST_CASE("全忙也要回一台，去排队而不是判失败") {
    // 一镜一两分钟等得起；当场失败的话，用户得到的是一集里随机几镜没了。
    const std::vector<NodeState> nodes = {
        node("x", true, {Capability::Video}, {}, true),
        node("y", true, {Capability::Video}, {}, true),
    };
    const auto got = pick_for(nodes, Capability::Video);
    REQUIRE(got.has_value());
    CHECK(*got == "x");
}

TEST_CASE("一台候选都没有才回空") {
    const std::vector<NodeState> nodes = {
        node("a", true, {Capability::Llm}),
    };
    CHECK_FALSE(pick_for(nodes, Capability::Video).has_value());
}

TEST_CASE("钉死某台 = 把别台关掉，走的是同一套代码") {
    // 「全自动」和「钉死某台」不该是两条路——分成两条，迟早在某个边角上
    // 行为不一致，而那种不一致查起来要先怀疑调度、再怀疑配置。
    std::vector<NodeState> nodes = {
        node("a", true, {Capability::Video}),
        node("b", true, {Capability::Video}),
    };
    CHECK(candidates_for(nodes, Capability::Video).size() == 2);

    nodes[0].off.insert(Capability::Video);   // 表上把 a 的出片关掉
    const auto cands = candidates_for(nodes, Capability::Video);
    REQUIRE(cands.size() == 1);
    CHECK(cands.front()->url == "b");
    CHECK(pick_for(nodes, Capability::Video) == "b");
}

TEST_CASE("派不出去时说清是哪一层拦的") {
    SUBCASE("一台都没登记") {
        const std::string why = why_no_node({}, Capability::Video);
        CHECK(why.find("没有任何一台") != std::string::npos);
    }
    SUBCASE("连不上") {
        const std::vector<NodeState> nodes = {
            node("a", false, {Capability::Video}),
        };
        const std::string why = why_no_node(nodes, Capability::Video);
        CHECK(why.find("连不上") != std::string::npos);
    }
    SUBCASE("你自己关的") {
        const std::vector<NodeState> nodes = {
            node("a", true, {Capability::Video}, {Capability::Video}),
        };
        const std::string why = why_no_node(nodes, Capability::Video);
        CHECK(why.find("关掉") != std::string::npos);
    }
    SUBCASE("它自己说干不了") {
        const std::vector<NodeState> nodes = {
            node("a", true, {Capability::Llm}),
        };
        const std::string why = why_no_node(nodes, Capability::Video);
        CHECK(why.find("干不了") != std::string::npos);
    }
    SUBCASE("三种都有就三种都说") {
        const std::vector<NodeState> nodes = {
            node("a", false, {Capability::Video}),
            node("b", true, {Capability::Video}, {Capability::Video}),
            node("c", true, {Capability::Llm}),
        };
        const std::string why = why_no_node(nodes, Capability::Video);
        CHECK(why.find("连不上") != std::string::npos);
        CHECK(why.find("关掉") != std::string::npos);
        CHECK(why.find("干不了") != std::string::npos);
    }
}

TEST_CASE("那句话里带着能力的中文名") {
    // 它会直接显示给用户，"video 这一步派不出去"没人看得懂。
    const std::string why = why_no_node({}, Capability::Tts);
    CHECK(why.find("配音") != std::string::npos);
}
