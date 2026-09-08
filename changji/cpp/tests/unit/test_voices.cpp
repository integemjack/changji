// GET /api/voices 的测试。
//
// 这个接口的价值在于"别让人手打一条路径然后在跑到配音那一步才发现填错"。
// 所以每一条失败路径都要**说清是哪一步失败的**——回一个空下拉框
// 等于把问题原样退回给用户。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "comfy/loader.hpp"
#include "http/voices.hpp"
#include "util/paths.hpp"

using namespace changji;
using comfy::OrderedJson;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

fs::path temp_root(const std::string& tag) {
    const fs::path d =
        fs::temp_directory_path() / paths::from_utf8("changji_音色_" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

models::ProjectStore make_store(const std::string& tag) {
    return models::ProjectStore::create(temp_root(tag), "yu_ye", "雨夜天台");
}

void write_file(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    f << text;
}

/// object_info 里挂一个带音色下拉框的节点。
OrderedJson tts_object_info(const std::vector<std::string>& voices) {
    OrderedJson options = OrderedJson::array();
    for (const auto& v : voices) options.push_back(v);
    return OrderedJson{
        {"TTSAudioSuite", {{"input", {{"required", {
            {"text", OrderedJson::array({"STRING"})},
            {"narrator_voice", OrderedJson::array({options})},
        }}}}}},
    };
}

comfy::Client make_client(const OrderedJson& object_info, bool reachable = true) {
    comfy::Transport t;
    t.get = [object_info, reachable](const std::string& path,
                                     double) -> comfy::HttpResponse {
        if (!reachable) return {0, "", "connection refused"};
        if (path == "/object_info") {
            return {200, object_info.dump(), std::nullopt};
        }
        return {404, "{}", std::nullopt};
    };
    return comfy::Client([] { return config::ComfyConfig{}; }, t, "cid");
}

}  // namespace

TEST_CASE("音色靠输入名认，不靠节点类型认") {
    // TTS 那一片的自定义节点包换得很勤，类型名各不相同，而输入名反而稳定。
    // 按类型名列白名单的话，用户换一个节点包音色列表就空了。
    const comfy::ApiWorkflow w(OrderedJson{
        {"1", {{"class_type", "谁也没听过的节点"},
               {"inputs", {{"text", "你好"}, {"speaker", "旁白"}}}}},
    });
    const auto found = http::find_voice_input(w);
    REQUIRE(found.has_value());
    CHECK(found->first == "谁也没听过的节点");
    CHECK(found->second == "speaker");
}

TEST_CASE("按节点顺序找，不是按键名顺序找") {
    // 反过来的话优先级就成了"哪个键名靠前"，一个工作流里有两个 TTS 节点时
    // 挑中的是另一个——而两个节点的音色列表可能完全不同。
    const comfy::ApiWorkflow w(OrderedJson{
        {"1", {{"class_type", "第一个"}, {"inputs", {{"speaker", "a"}}}}},
        {"2", {{"class_type", "第二个"}, {"inputs", {{"voice", "b"}}}}},
    });
    const auto found = http::find_voice_input(w);
    REQUIRE(found.has_value());
    CHECK(found->first == "第一个");   // 不是"第二个"，尽管 voice 在名单里更靠前
}

TEST_CASE("一个音色输入都没有时返回空") {
    const comfy::ApiWorkflow w(OrderedJson{
        {"1", {{"class_type", "KSampler"}, {"inputs", {{"steps", 20}}}}},
    });
    CHECK_FALSE(http::find_voice_input(w).has_value());
}

TEST_CASE("正常拿到音色列表") {
    const auto store = make_store("正常");
    write_file(store.root() / "workflows" / "tts.json",
               OrderedJson{{"1", {{"class_type", "TTSAudioSuite"},
                                  {"inputs", {{"text", ""},
                                              {"narrator_voice", "none"}}}}}}.dump());

    auto client = make_client(tts_object_info({"none", "林晚.wav", "旁白.wav"}));
    const auto r = http::get_voices(paths::to_utf8(store.root()), client);

    CHECK(r.status == 200);
    CHECK(r.body["voices"] == json::array({"林晚.wav", "旁白.wav"}));
    // 成功时不该带 error 字段
    CHECK_FALSE(r.body.contains("error"));
}

TEST_CASE("none 和空串要滤掉") {
    // "none" 是节点用来表示"不指定"的占位值，不是一个音色。
    // 留着的话下拉框里多出一项，选了等于没选。
    const auto store = make_store("过滤");
    write_file(store.root() / "workflows" / "tts.json",
               OrderedJson{{"1", {{"class_type", "TTSAudioSuite"},
                                  {"inputs", {{"narrator_voice", "none"}}}}}}.dump());

    auto client = make_client(tts_object_info({"none", "", "真的音色"}));
    const auto r = http::get_voices(paths::to_utf8(store.root()), client);
    CHECK(r.body["voices"] == json::array({"真的音色"}));
}

TEST_CASE("服务端连不上时说清楚，不是回一个空下拉框") {
    // **这条和 Python 不一样，是有意的。** 那边把异常吞掉返回空列表，
    // 于是"服务端连不上"和"一个音色都没装"回的是同一个空列表——
    // 用户看到的是一个空下拉框，没有任何线索。
    const auto store = make_store("连不上");
    write_file(store.root() / "workflows" / "tts.json",
               OrderedJson{{"1", {{"class_type", "TTSAudioSuite"},
                                  {"inputs", {{"narrator_voice", "none"}}}}}}.dump());

    auto client = make_client(OrderedJson::object(), false);
    const auto r = http::get_voices(paths::to_utf8(store.root()), client);

    CHECK(r.status == 200);   // 不是 500：角色页顺带拉的，不该弹错误框
    CHECK(r.body["voices"].empty());
    REQUIRE(r.body.contains("error"));
    const std::string err = r.body["error"];
    CAPTURE(err);
    CHECK(err.find("服务端") != std::string::npos);
}

TEST_CASE("工作流里没有音色输入时说该去哪儿改") {
    const auto store = make_store("没输入");
    write_file(store.root() / "workflows" / "tts.json",
               OrderedJson{{"1", {{"class_type", "KSampler"},
                                  {"inputs", {{"steps", 20}}}}}}.dump());

    auto client = make_client(tts_object_info({"a"}));
    const auto r = http::get_voices(paths::to_utf8(store.root()), client);
    REQUIRE(r.body.contains("error"));
    const std::string err = r.body["error"];
    CAPTURE(err);
    CHECK(err.find("tts.json") != std::string::npos);
}

TEST_CASE("项目路径为空是 400") {
    auto client = make_client(OrderedJson::object());
    CHECK_THROWS_AS(http::get_voices("", client), http::ApiError);
}

TEST_CASE("没有项目文件时用内置的 tts 工作流") {
    // 内置那份编在二进制里，所以一个刚建好的项目也能列音色。
    const auto store = make_store("内置");
    auto client = make_client(tts_object_info({"内置也能列"}));
    const auto r = http::get_voices(paths::to_utf8(store.root()), client);
    CAPTURE(r.body.dump());
    // 内置的 tts.json 用的是哪个节点由它自己决定，这里只要求别报
    // "读不到配音工作流"——那说明内置那份根本没被找到。
    if (r.body.contains("error")) {
        const std::string err = r.body["error"];
        CHECK(err.find("读不到配音工作流") == std::string::npos);
    }
}

TEST_CASE("进程内配音：不去问 ComfyUI，直说音色来自参考音频") {
    // **答非所问比答不出来更糟。** 选了 local 的用户如果在角色页看到
    // 一个 ComfyUI 的音色下拉框（或者一句"连不上 ComfyUI"），
    // 他会去查 ComfyUI——而那个后端压根没在用。
    //
    // 这里传一个会抛异常的假客户端：真去问了就会炸，
    // 用例通过本身就证明了它没去问。
    // 这个 transport 一被碰就让用例失败：**通过本身就证明了它没去问。**
    comfy::Transport t;
    t.get = [](const std::string& path, double) -> comfy::HttpResponse {
        FAIL("backend=local 时不该去问 ComfyUI，却请求了 " << path);
        return {0, "", "unreachable"};
    };
    comfy::Client client([] { return config::ComfyConfig{}; }, t, "cid");

    const auto r = http::get_voices("任意路径", client, "local");
    CHECK(r.status == 200);
    CHECK(r.body.at("voices").empty());
    const std::string err = r.body.at("error").get<std::string>();
    CHECK(err.find("参考音频") != std::string::npos);
}
