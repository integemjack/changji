// 工作流加载的测试。
//
// 两条规则各有代价：
//   项目覆盖内置——不成立的话"不同的剧用不同的模型"就没了；
//   内置编进二进制——不成立的话，"exe 拷过去了、workflows 目录忘了拷"
//   会在跑到出片那一步才报错，而前面几十分钟的配音和首帧已经跑完了。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "comfy/loader.hpp"
#include "util/paths.hpp"

using namespace changji;
using comfy::OrderedJson;
namespace fs = std::filesystem;

namespace {

fs::path temp_root(const std::string& tag) {
    const fs::path d =
        fs::temp_directory_path() / paths::from_utf8("changji_工作流_" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

models::ProjectStore make_store(const std::string& tag) {
    return models::ProjectStore::create(temp_root(tag), "yu_ye", "雨夜天台");
}

OrderedJson corpus_object_info() {
    const fs::path p =
        fs::path(CHANGJI_GOLDEN_DIR) / "comfy" / "workflow_convert.json";
    std::ifstream in(p, std::ios::binary);
    REQUIRE(in.good());
    std::ostringstream buf;
    buf << in.rdbuf();
    return OrderedJson::parse(buf.str()).at("object_info");
}

/// 一个只会回 object_info 的假服务端。别的路径一律"连不上"——
/// 这样"这条路不该碰服务端"是能被测出来的，而不是碰巧没碰。
comfy::Client make_client(const OrderedJson& object_info, int* hits = nullptr) {
    comfy::Transport t;
    t.get = [object_info, hits](const std::string& path,
                                double) -> comfy::HttpResponse {
        if (path == "/object_info") {
            if (hits) ++*hits;
            return {200, object_info.dump(), std::nullopt};
        }
        return {0, "", "这条路不该发请求"};
    };
    return comfy::Client([] { return config::ComfyConfig{}; }, t, "cid");
}

void write_file(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    f << text;
}

}  // namespace

TEST_CASE("内置工作流编在二进制里") {
    // 随程序装几个 json 的话，"exe 拷过去了、workflows 目录忘了拷"
    // 会在跑到出片那一步才报错。
    const auto video = comfy::bundled_workflow("video");
    REQUIRE(video.has_value());
    CHECK(video->find("Wan22ImageToVideoLatent") != std::string::npos);

    CHECK(comfy::bundled_workflow("tts").has_value());
    CHECK_FALSE(comfy::bundled_workflow("image").has_value());
    CHECK_FALSE(comfy::bundled_workflow("根本没有").has_value());

    SUBCASE("内容是合法 JSON 而且是界面版") {
        const OrderedJson j = OrderedJson::parse(*video, nullptr, false);
        REQUIRE_FALSE(j.is_discarded());
        CHECK(j.contains("nodes"));
    }
}

TEST_CASE("没有项目文件时用内置的") {
    const auto store = make_store("内置");
    int hits = 0;
    auto client = make_client(corpus_object_info(), &hits);

    const auto w = comfy::load_workflow(client, store, "video");
    REQUIRE(w.has_value());
    // 内置的是界面版，转出来应该有那个 KSampler
    CHECK(w->find_by_class("KSampler").size() == 1);
    CHECK(hits == 1);   // 界面版才拉 object_info
}

TEST_CASE("项目里的同名文件覆盖内置的") {
    // 不同的剧用不同的模型靠的就是这个。
    const auto store = make_store("覆盖");
    write_file(store.root() / "workflows" / "video.json",
               OrderedJson{{"1", {{"class_type", "我自己的节点"},
                                  {"inputs", OrderedJson::object()}}}}.dump());

    int hits = 0;
    auto client = make_client(corpus_object_info(), &hits);
    const auto w = comfy::load_workflow(client, store, "video");
    REQUIRE(w.has_value());
    CHECK(w->find_by_class("我自己的节点").size() == 1);
    CHECK(w->find_by_class("KSampler").empty());   // 内置那份没被用上
}

TEST_CASE("接口版工作流不碰服务端") {
    // 这条很要紧：转换要拉 /object_info，而接口版根本不需要服务端在线。
    // 无脑先拉一次的话，ComfyUI 没起时连一份现成的接口版工作流都读不了。
    const auto store = make_store("接口版");
    write_file(store.root() / "workflows" / "video.json",
               OrderedJson{{"9", {{"class_type", "KSampler"},
                                  {"inputs", {{"steps", 20}}}}}}.dump());

    int hits = 0;
    auto client = make_client(corpus_object_info(), &hits);
    const auto w = comfy::load_workflow(client, store, "video");
    REQUIRE(w.has_value());
    CHECK(w->get_input("9", "steps") == 20);
    CHECK(hits == 0);
}

TEST_CASE("非必需的工作流找不到就返回空，不抛") {
    // 没有图像工作流就用视频模型出单帧，没有配音工作流就只算时长。
    // 抛的话整条流水线因为一个可选项跑不起来。
    const auto store = make_store("可选");
    auto client = make_client(corpus_object_info());
    CHECK_FALSE(comfy::load_workflow(client, store, "image", false).has_value());
}

TEST_CASE("必需的工作流找不到时说清放哪儿") {
    // 只说"找不到"的话用户不知道该把文件放到哪个目录、叫什么名字。
    const auto store = make_store("必需");
    auto client = make_client(corpus_object_info());
    try {
        comfy::load_workflow(client, store, "根本没有这个", true);
        FAIL("该抛");
    } catch (const comfy::WorkflowError& e) {
        const std::string msg = e.what();
        CAPTURE(msg);
        CHECK(msg.find("workflows/根本没有这个.json") != std::string::npos);
    }
}

TEST_CASE("load_all 只把视频当必需的") {
    const auto store = make_store("全部");
    auto client = make_client(corpus_object_info());
    const auto all = comfy::load_all(client, store);

    REQUIRE(all.count("video") == 1);
    CHECK(all.at("video").has_value());
    // image 没有内置的，缺了是正常的
    CHECK_FALSE(all.at("image").has_value());
    // tts 有内置的
    CHECK(all.at("tts").has_value());
}

TEST_CASE("项目里的文件不是合法 JSON 时带上路径") {
    const auto store = make_store("坏文件");
    write_file(store.root() / "workflows" / "video.json", "{ 这不是 JSON");
    auto client = make_client(corpus_object_info());
    try {
        comfy::load_workflow(client, store, "video");
        FAIL("该抛");
    } catch (const comfy::WorkflowError& e) {
        CHECK(std::string(e.what()).find("video.json") != std::string::npos);
    }
}
