// 接外来的活之前那道门。
//
// 这一组用例守的是两条不对称的规矩：**放行错了没人知道，拦错了当场就知道。**
// 所以每一条判断都往安全那边倒——认不出的监听地址算对外，没设口令算不接。

#include <doctest/doctest.h>

#include "infer/peer_auth.hpp"

using namespace changji::infer;

TEST_CASE("回环地址不算对外") {
    CHECK_FALSE(is_public_bind("127.0.0.1"));
    CHECK_FALSE(is_public_bind("localhost"));
    CHECK_FALSE(is_public_bind("::1"));
    CHECK_FALSE(is_public_bind("[::1]"));
    CHECK_FALSE(is_public_bind("127.0.1.5"));   // 整段 127.x 都是
    CHECK_FALSE(is_public_bind("  127.0.0.1 "));
    CHECK_FALSE(is_public_bind("LOCALHOST"));
}

TEST_CASE("认不出来的一律算对外") {
    // 猜错的方向要挑代价小的那个：把回环误判成对外，症状是"你得设个口令"；
    // 反过来是门开着而没人知道。
    CHECK(is_public_bind("0.0.0.0"));
    CHECK(is_public_bind("::"));
    CHECK(is_public_bind("192.168.1.7"));
    CHECK(is_public_bind("1.2.3.4"));
    CHECK(is_public_bind(""));   // 没说 = Crow 听 0.0.0.0
}

TEST_CASE("对外监听没设口令就不许起") {
    const std::string why = refuse_to_listen("0.0.0.0", "");
    CHECK_FALSE(why.empty());
    // 那句话要说到"该怎么办"，不能只说"不允许"
    CHECK(why.find("127.0.0.1") != std::string::npos);
    CHECK(why.find("[peer].token") != std::string::npos);
}

TEST_CASE("本机多卡那条路不受影响") {
    // 自己按显卡数拉起的工作进程监听回环，一行配置都不用改。
    CHECK(refuse_to_listen("127.0.0.1", "").empty());
    CHECK(refuse_to_listen("127.0.0.1", "随便").empty());
    CHECK(refuse_to_listen("0.0.0.0", "有口令").empty());
}

TEST_CASE("没设口令就不接外来的活") {
    // 不是"谁都能接"。这是这组里最要紧的一条。
    CHECK_FALSE(token_ok("Bearer 什么都行", ""));
    CHECK_FALSE(token_ok("", ""));
    CHECK_FALSE(token_ok("Bearer ", "   "));   // 只有空白也算没设
}

TEST_CASE("口令对得上才放行") {
    CHECK(token_ok("Bearer s3cret", "s3cret"));
    CHECK(token_ok("bearer s3cret", "s3cret"));   // 前缀大小写不敏感
    CHECK(token_ok("BEARER s3cret", "s3cret"));
    CHECK(token_ok("  Bearer  s3cret  ", "s3cret"));  // 粘贴时带的空白

    CHECK_FALSE(token_ok("Bearer s3cre", "s3cret"));    // 短一个字符
    CHECK_FALSE(token_ok("Bearer s3crett", "s3cret"));  // 长一个字符
    CHECK_FALSE(token_ok("Bearer S3cret", "s3cret"));   // 口令本身大小写敏感
    CHECK_FALSE(token_ok("s3cret", "s3cret"));          // 少了 Bearer
    CHECK_FALSE(token_ok("Basic s3cret", "s3cret"));    // 换了方案
    CHECK_FALSE(token_ok("Bearer", "s3cret"));          // 只有前缀
}

TEST_CASE("口令两端的空白不算数") {
    // 从界面上复制口令，末尾带个换行是常事——那不该是"口令不对"。
    CHECK(token_ok("Bearer abc", "  abc  "));
    CHECK(token_ok("Bearer abc" "\n", "abc"));
}
