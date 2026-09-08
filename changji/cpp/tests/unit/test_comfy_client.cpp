// ComfyUI 客户端的测试。
//
// 传输全是注入的，所以这里能把最难验的那部分测死：**什么时候算跑完**。
// 真连一台 ComfyUI 的话，"完成消息没送到"这种情况要靠运气才撞得上，
// 而它在量产时天天发生（同一个 clientId 上并发跑两个任务，
// 后连的把先连的挤下线）。
//
// 另一条同样重要：**哪一类错误该重试**。校验失败重试是纯浪费——
// 打错一个模型文件名，界面要转四分钟才报错。

#include <doctest/doctest.h>

#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "comfy/client.hpp"
#include "util/paths.hpp"

using namespace changji;
using comfy::OrderedJson;
namespace fs = std::filesystem;

namespace {

/// 一台脚本化的假 ComfyUI。
struct FakeServer {
    /// path -> 依次要回的响应。用光了就重复最后一条。
    std::map<std::string, std::vector<comfy::HttpResponse>> gets;
    std::vector<comfy::HttpResponse> post_replies;
    /// WebSocket 依次吐的消息。nullopt 表示"这一轮没消息"（等超时）。
    std::deque<std::optional<std::string>> ws_messages;
    /// connect_ws 返回空，模拟连不上
    bool ws_unavailable = false;

    // 记录
    std::vector<std::string> get_paths;
    std::vector<std::string> post_bodies;
    std::vector<double> slept;
    std::string ws_url;

    static comfy::HttpResponse ok(const std::string& body) {
        return {200, body, std::nullopt};
    }
    static comfy::HttpResponse code(int c, const std::string& body) {
        return {c, body, std::nullopt};
    }
    static comfy::HttpResponse dead(const std::string& why) {
        return {0, "", why};
    }

    comfy::HttpResponse take_get(const std::string& path) {
        get_paths.push_back(path);
        auto it = gets.find(path);
        if (it == gets.end() || it->second.empty()) {
            return code(404, "{}");
        }
        if (it->second.size() == 1) return it->second.front();
        comfy::HttpResponse r = it->second.front();
        it->second.erase(it->second.begin());
        return r;
    }

    comfy::Transport transport() {
        comfy::Transport t;
        t.get = [this](const std::string& p, double) { return take_get(p); };
        t.post_json = [this](const std::string& p, const std::string& b, double) {
            post_bodies.push_back(b);
            if (post_replies.empty()) return ok(R"({"prompt_id":"pid-1"})");
            comfy::HttpResponse r = post_replies.front();
            if (post_replies.size() > 1) post_replies.erase(post_replies.begin());
            (void)p;
            return r;
        };
        t.upload = [this](const std::string&, const fs::path&,
                          const std::string&, double) {
            if (post_replies.empty()) {
                return ok(R"({"name":"a.png","subfolder":""})");
            }
            return post_replies.front();
        };
        t.download = [](const std::string&,
                        const std::map<std::string, std::string>&,
                        const fs::path& dest, double) {
            std::ofstream f(dest, std::ios::binary);
            f << "假的产出";
            return ok("");
        };
        t.connect_ws = [this](const std::string& url) -> comfy::WsRecv {
            ws_url = url;
            if (ws_unavailable) return nullptr;
            return [this](double) -> std::optional<std::string> {
                if (ws_messages.empty()) return std::nullopt;
                auto m = ws_messages.front();
                ws_messages.pop_front();
                return m;
            };
        };
        t.sleep = [this](double s) { slept.push_back(s); };
        return t;
    }
};

comfy::ConfigProvider fast_config() {
    return [] {
        config::ComfyConfig c;
        c.job_timeout_s = 30.0;
        c.max_retries = 2;
        return c;
    };
}

std::string ws_msg(const std::string& type, const OrderedJson& data) {
    return OrderedJson{{"type", type}, {"data", data}}.dump();
}

/// 一份跑完了的 history。
std::string done_history(const std::string& pid) {
    return OrderedJson{{pid, {
        {"status", {{"status_str", "success"}, {"completed", true}}},
        {"outputs", {{"12", {{"images", OrderedJson::array({
            OrderedJson{{"filename", "out.mp4"}, {"subfolder", "video"},
                        {"type", "output"}, {"animated", true}}})}}}}},
    }}}.dump();
}

comfy::ApiWorkflow tiny_workflow() {
    return comfy::ApiWorkflow(OrderedJson{
        {"1", {{"class_type", "VAEDecode"}, {"inputs", OrderedJson::object()}}}});
}

}  // namespace

TEST_CASE("跑完的信号是 executing 且 node 为 null") {
    // 这是最容易看漏的一条：它和"正在执行某个节点"是同一个消息类型，
    // 只差 node 是不是 null。判错的话任务永远等不到结束。
    FakeServer s;
    s.gets["/history/pid-1"] = {FakeServer::ok(done_history("pid-1"))};
    s.ws_messages = {
        ws_msg("executing", {{"prompt_id", "pid-1"}, {"node", "9"}}),
        ws_msg("progress", {{"prompt_id", "pid-1"}, {"node", "9"},
                            {"value", 12}, {"max", 30}}),
        ws_msg("executing", {{"prompt_id", "pid-1"}, {"node", nullptr}}),
    };

    std::vector<comfy::JobProgress> seen;
    comfy::Client c(fast_config(), s.transport(), "cid");
    pipeline::CancelToken tok;
    const auto r = c.wait("pid-1", [&](const comfy::JobProgress& p) {
        seen.push_back(p);
    }, 5.0, "cid", tok);

    CHECK(r.prompt_id == "pid-1");
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].step == 12);
    CHECK(seen[0].total == 30);
    CHECK(seen[0].fraction() == doctest::Approx(0.4));

    SUBCASE("视频产出也在 images 键下") {
        // 按 videos 找的话一个都找不到，而"没有产出"和"跑失败了"
        // 在上层是同一个表现。
        const auto files = r.files();
        REQUIRE(files.size() == 1);
        CHECK(files[0]["filename"] == "out.mp4");
        CHECK(r.first_file().has_value());
    }
}

TEST_CASE("完成消息没送到时靠查历史兜底") {
    // 任务在 WebSocket 连上之前就结束了，或者同一个 clientId 上并发跑了
    // 两个任务、后连的把先连的挤下线。不兜底的话这里白等到 job_timeout_s，
    // 默认是半小时。
    FakeServer s;
    s.gets["/history/pid-1"] = {FakeServer::ok(done_history("pid-1"))};
    s.ws_messages = {std::nullopt};   // 一条消息都没有，直接等超时

    comfy::Client c(fast_config(), s.transport(), "cid");
    pipeline::CancelToken tok;
    const auto r = c.wait("pid-1", nullptr, 5.0, "cid", tok);
    CHECK(r.files().size() == 1);
}

TEST_CASE("兜底查询查到任务还没跑完时接着等") {
    // 把"历史里还没有这一条"当成跑完了的话，会返回一个空结果，
    // 而上层看到的是"任务成功了但没有产出文件"——比报错更难查。
    FakeServer s;
    s.gets["/history/pid-1"] = {
        FakeServer::ok("{}"),                        // 还没进历史
        FakeServer::ok(OrderedJson{{"pid-1", {
            {"status", {{"status_str", "running"}, {"completed", false}}},
            {"outputs", OrderedJson::object()}}}}.dump()),   // 在跑
        FakeServer::ok(done_history("pid-1")),       // 跑完了
    };
    s.ws_messages = {std::nullopt, std::nullopt, std::nullopt};

    comfy::Client c(fast_config(), s.transport(), "cid");
    pipeline::CancelToken tok;
    const auto r = c.wait("pid-1", nullptr, 5.0, "cid", tok);
    CHECK(r.files().size() == 1);
}

TEST_CASE("老版本不带 completed 字段时不能当成没跑完") {
    // 当成 false 的话兜底查询永远返回"还没跑完"，白等到超时。
    FakeServer s;
    s.gets["/history/pid-1"] = {FakeServer::ok(OrderedJson{{"pid-1", {
        {"status", {{"status_str", "success"}}},   // 没有 completed
        {"outputs", {{"9", {{"images", OrderedJson::array({
            OrderedJson{{"filename", "a.png"}}})}}}}},
    }}}.dump())};
    s.ws_messages = {std::nullopt};

    comfy::Client c(fast_config(), s.transport(), "cid");
    pipeline::CancelToken tok;
    const auto r = c.wait("pid-1", nullptr, 5.0, "cid", tok);
    CHECK(r.files().size() == 1);
}

TEST_CASE("别人的任务的消息要滤掉") {
    // 同一条 WebSocket 上会有别的客户端的消息。不滤的话，别人的任务
    // 跑完会让这里以为自己跑完了，然后去查一个还没有产出的历史。
    FakeServer s;
    s.gets["/history/pid-1"] = {FakeServer::ok(done_history("pid-1"))};
    s.ws_messages = {
        ws_msg("executing", {{"prompt_id", "别人的"}, {"node", nullptr}}),
        ws_msg("progress", {{"prompt_id", "别人的"}, {"value", 5}, {"max", 5}}),
        ws_msg("executing", {{"prompt_id", "pid-1"}, {"node", nullptr}}),
    };

    std::vector<comfy::JobProgress> seen;
    comfy::Client c(fast_config(), s.transport(), "cid");
    pipeline::CancelToken tok;
    c.wait("pid-1", [&](const comfy::JobProgress& p) { seen.push_back(p); },
           5.0, "cid", tok);
    CHECK(seen.empty());   // 别人的进度不该报给我们的界面
}

TEST_CASE("执行失败要说清是哪个节点") {
    // 只说"任务失败"的话，一个二十个节点的工作流无从查起。
    FakeServer s;
    s.ws_messages = {ws_msg("execution_error", {
        {"prompt_id", "pid-1"},
        {"node_type", "KSampler"},
        {"exception_message", "CUDA out of memory"}})};

    comfy::Client c(fast_config(), s.transport(), "cid");
    pipeline::CancelToken tok;
    try {
        c.wait("pid-1", nullptr, 5.0, "cid", tok);
        FAIL("该抛");
    } catch (const comfy::ExecutionError& e) {
        const std::string msg = e.what();
        CAPTURE(msg);
        CHECK(msg.find("KSampler") != std::string::npos);
        CHECK(msg.find("out of memory") != std::string::npos);
    }
}

TEST_CASE("WebSocket 连不上时退回轮询") {
    // 反向代理不转发 WS 升级是很常见的部署问题。为它整条路都跑不了不值得。
    FakeServer s;
    s.ws_unavailable = true;
    s.gets["/history/pid-1"] = {FakeServer::ok(done_history("pid-1"))};

    comfy::Client c(fast_config(), s.transport(), "cid");
    pipeline::CancelToken tok;
    const auto r = c.wait("pid-1", nullptr, 5.0, "cid", tok);
    CHECK(r.files().size() == 1);
}

TEST_CASE("轮询时历史里报错也要抛") {
    // messages 是 [["execution_error", {...}]] 这种数组套数组，
    // 取错一层就只剩"任务执行失败"，那句话对排查没有任何帮助。
    FakeServer s;
    s.ws_unavailable = true;
    s.gets["/history/pid-1"] = {FakeServer::ok(OrderedJson{{"pid-1", {
        {"status", {{"status_str", "error"},
                    {"messages", OrderedJson::array({
                        OrderedJson::array({"execution_start", OrderedJson::object()}),
                        OrderedJson::array({"execution_error", {
                            {"node_type", "LoadImage"},
                            {"exception_message", "文件不存在"}}}),
                    })}}},
    }}}.dump())};

    comfy::Client c(fast_config(), s.transport(), "cid");
    pipeline::CancelToken tok;
    try {
        c.wait("pid-1", nullptr, 5.0, "cid", tok);
        FAIL("该抛");
    } catch (const comfy::ExecutionError& e) {
        const std::string msg = e.what();
        CAPTURE(msg);
        CHECK(msg.find("LoadImage") != std::string::npos);
        CHECK(msg.find("文件不存在") != std::string::npos);
    }

    SUBCASE("没有 execution_error 那条时也有话说") {
        const std::string s2 = comfy::error_from_history(
            OrderedJson{{"messages", OrderedJson::array()}});
        CHECK_FALSE(s2.empty());
    }
}

TEST_CASE("校验失败不重试，执行失败才重试") {
    // **这条是这个文件里最要紧的一条。** 混在一起重试的表现是
    // "模型文件名打错一个字母，界面转了四分钟才报错"。
    SUBCASE("400 是校验失败，一次就抛") {
        FakeServer s;
        s.post_replies = {FakeServer::code(400, OrderedJson{
            {"error", {{"message", "Prompt has no outputs"}}},
            {"node_errors", OrderedJson::object()}}.dump())};

        comfy::Client c(fast_config(), s.transport(), "cid");
        pipeline::CancelToken tok;
        CHECK_THROWS_AS(c.run(tiny_workflow(), nullptr, tok),
                        comfy::PromptValidationError);
        CHECK(s.post_bodies.size() == 1);   // 只提交了一次
        CHECK(s.slept.empty());             // 一次都没退避
    }

    SUBCASE("连不上是可重试的") {
        FakeServer s;
        s.post_replies = {FakeServer::dead("connection refused")};

        comfy::Client c(fast_config(), s.transport(), "cid");
        pipeline::CancelToken tok;
        CHECK_THROWS_AS(c.run(tiny_workflow(), nullptr, tok),
                        comfy::ExecutionError);
        CHECK(s.post_bodies.size() == 3);   // max_retries=2，共三次
        // 指数退避：1、2 秒
        REQUIRE(s.slept.size() == 2);
        CHECK(s.slept[0] == doctest::Approx(1.0));
        CHECK(s.slept[1] == doctest::Approx(2.0));
    }
}

TEST_CASE("每个任务用独立的 clientId") {
    // ComfyUI 按 clientId 记订阅。同一个 id 上并发跑两个任务，
    // 后连的会把先连的挤下线，先连的永远等不到完成消息。
    FakeServer s;
    s.gets["/history/pid-1"] = {FakeServer::ok(done_history("pid-1"))};
    s.ws_messages = {ws_msg("executing", {{"prompt_id", "pid-1"},
                                          {"node", nullptr}})};

    comfy::Client c(fast_config(), s.transport(), "cid");
    pipeline::CancelToken tok;
    c.run(tiny_workflow(), nullptr, tok);

    REQUIRE(s.post_bodies.size() == 1);
    const OrderedJson body = OrderedJson::parse(s.post_bodies[0]);
    const std::string used = body.at("client_id");
    CAPTURE(used);
    CHECK(used != "cid");                     // 不是那个基础 id
    CHECK(used.rfind("cid-", 0) == 0);        // 但看得出是它派生的
    // WebSocket 订阅的必须是同一个，否则消息压根收不到
    CHECK(s.ws_url.find("clientId=" + used) != std::string::npos);
}

TEST_CASE("校验错误要翻成人话") {
    // 原样透传的话用户看到的是一坨嵌套 JSON，而里面真正有用的只有
    // "哪个节点要的哪个文件服务端上没有"。
    const comfy::PromptValidationError e("工作流校验失败", OrderedJson{
        {"1", {{"class_type", "UNETLoader"},
               {"errors", OrderedJson::array({OrderedJson{
                   {"message", "Value not in list: unet_name"},
                   {"extra_info", {{"input_name", "unet_name"},
                                   {"received_value", "wan2.2_ti2v.safetensors"}}}}})}}},
    });

    const std::string s = e.human_summary();
    CAPTURE(s);
    CHECK(s.find("UNETLoader") != std::string::npos);
    CHECK(s.find("wan2.2_ti2v.safetensors") != std::string::npos);
    CHECK(s.find("服务端上没有这个文件") != std::string::npos);

    SUBCASE("没有 node_errors 时退回原消息，不是空串") {
        const comfy::PromptValidationError bare("提交被拒绝", OrderedJson::object());
        CHECK(bare.human_summary() == "提交被拒绝");
    }
}

TEST_CASE("取消要能打断等待") {
    // 打不断的话，点停止之后还要等到 job_timeout_s——默认半小时。
    FakeServer s;
    s.ws_messages = {std::nullopt};
    s.gets["/history/pid-1"] = {FakeServer::ok("{}")};

    comfy::Client c(fast_config(), s.transport(), "cid");
    pipeline::CancelToken tok;
    tok.request();
    CHECK_THROWS_AS(c.wait("pid-1", nullptr, 5.0, "cid", tok),
                    comfy::ExecutionError);
}

TEST_CASE("提交前就能查出服务端有哪些模型") {
    // 提交之后才发现的话，报错要从 node_errors 里翻出来。
    FakeServer s;
    s.gets["/object_info"] = {FakeServer::ok(OrderedJson{
        {"UNETLoader", {{"input", {{"required", {
            {"unet_name", OrderedJson::array({OrderedJson::array(
                {"a.safetensors", "b.safetensors"})})},
            {"weight_dtype", OrderedJson::array({OrderedJson::array({"default"})})},
        }}}}}},
    }.dump())};

    comfy::Client c(fast_config(), s.transport(), "cid");
    const auto models = c.available_models("UNETLoader", "unet_name");
    REQUIRE(models.size() == 2);
    CHECK(models[0] == "a.safetensors");

    SUBCASE("object_info 只拉一次") {
        // 一份 object_info 是几百 KB，每个镜头拉一次是纯浪费。
        c.available_models("UNETLoader", "weight_dtype");
        c.converter();
        int hits = 0;
        for (const auto& p : s.get_paths) {
            if (p == "/object_info") ++hits;
        }
        CHECK(hits == 1);
    }

    SUBCASE("问一个不存在的节点或输入回空表，不抛") {
        CHECK(c.available_models("根本没有", "x").empty());
        CHECK(c.available_models("UNETLoader", "根本没有").empty());
    }
}

TEST_CASE("上传要用服务端回的文件名") {
    // 服务端可能把文件放进了别的子目录（重名时）。用本地文件名的话，
    // 工作流引用的是一个不存在的路径——而那要等提交时才报错。
    const fs::path dir =
        fs::temp_directory_path() / paths::from_utf8("changji_上传");
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path f = dir / paths::from_utf8("首帧.png");
    { std::ofstream o(f, std::ios::binary); o << "假的"; }

    FakeServer s;
    s.post_replies = {FakeServer::ok(
        R"({"name":"首帧 (2).png","subfolder":"changji"})")};

    comfy::Client c(fast_config(), s.transport(), "cid");
    CHECK(c.upload_image(f) == "changji/首帧 (2).png");

    SUBCASE("文件不在时先报本地的错，不去连服务端") {
        CHECK_THROWS_AS(c.upload_image(dir / "没有这个.png"), comfy::ComfyError);
    }
}

TEST_CASE("ping 不抛，连不上就是 false") {
    // 体检那条路要的是"能不能用"，不是异常。
    FakeServer s;
    s.gets["/system_stats"] = {FakeServer::dead("connection refused")};
    comfy::Client c(fast_config(), s.transport(), "cid");
    CHECK_FALSE(c.ping());

    s.gets["/system_stats"] = {FakeServer::ok(R"({"system":{}})")};
    comfy::Client c2(fast_config(), s.transport(), "cid");
    CHECK(c2.ping());
}


// ---------------------------------------------------------------------------
// 和 Python 逐条比 client.py 里那几个纯函数。
//
// 上面那些用例钉的是我们自己的意图——`contract_audit.py` 一直把这个文件
// 列在「两边都有、又只钉了意图」那一类里。这一段把能脱开网络跑的三件事
// 拿去问 Python。
//
// 最要紧的是 **first_file()**：`outputs` 是「节点 id -> {images:[...]}」，
// 谁排在前面就决定成片用哪张图。Python 那边是 dict 走插入顺序；
// C++ 这边 `OrderedJson` 是 `nlohmann::ordered_json`，也是插入顺序——
// **这是有意选的**。哪天有人顺手换成 `nlohmann::json`，它按键名字典序，
// `"10"` 就跑到 `"9"` 前面，`first_file()` 换一张图**而且一声不吭**：
// 文件在、格式对、流程全绿，只是画面不对。
// 语料专门挑了插入序和字典序相反的节点 id，正反各一条。
// ---------------------------------------------------------------------------

namespace {

OrderedJson load_comfy_golden() {
    const std::string path =
        std::string(CHANGJI_GOLDEN_DIR) + "/comfy_client.json";
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "读不到语料 " << path);
    OrderedJson g;
    in >> g;
    return g;
}

}  // namespace

TEST_CASE("产出文件的挑选和 Python 一样（含插入顺序）") {
    const auto g = load_comfy_golden();
    const auto cases = g.at("outputs");
    REQUIRE(cases.size() == 9);

    for (const auto& c : cases) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);

        comfy::JobResult r;
        r.prompt_id = "p1";
        r.outputs = c.at("outputs");

        if (c.contains("divergent")) {
            // 两边有意不一样。**不比 Python 的值**——比了就是把
            // 其中一边的行为写进期望里假装是契约。这里钉 C++ 自己该做什么，
            // 理由在语料的 divergent 字段里，也写进了方案。
            CHECK(r.files().empty());
            CHECK_FALSE(r.first_file().has_value());
            continue;
        }

        CHECK(r.files() == c.at("files_images").get<std::vector<OrderedJson>>());
        CHECK(r.files("videos") ==
              c.at("files_videos").get<std::vector<OrderedJson>>());

        const auto want_first = c.at("first_file");
        if (want_first.is_null()) {
            CHECK_FALSE(r.first_file().has_value());
        } else {
            REQUIRE(r.first_file().has_value());
            CHECK(*r.first_file() == want_first);
        }
    }
}

TEST_CASE("校验失败翻成的人话和 Python 一字不差") {
    // 这句话是用户唯一能看懂的东西。翻错了没人会发现——
    // 用户只会觉得"报错看不懂"，然后去装一堆其实不缺的东西。
    const auto g = load_comfy_golden();
    const auto cases = g.at("validation");
    REQUIRE(cases.size() == 9);

    for (const auto& c : cases) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);
        const auto ne = c.at("node_errors");
        comfy::PromptValidationError e(
            c.at("message").get<std::string>(),
            ne.is_null() ? OrderedJson::object() : ne);
        CHECK(e.human_summary() == c.at("human_summary").get<std::string>());
    }
}

TEST_CASE("从 history 里挖执行失败的原因和 Python 一样") {
    const auto g = load_comfy_golden();
    const auto cases = g.at("history");
    REQUIRE(cases.size() == 8);

    for (const auto& c : cases) {
        const std::string name = c.at("name").get<std::string>();
        CAPTURE(name);
        const std::string got = comfy::error_from_history(c.at("status"));

        if (c.contains("divergent")) {
            // Python 把它自己的 None 渲染进中文句子里（"节点 None 执行失败"）。
            // C++ 渲染成 "?"——都没信息，但 ? 不会让人以为有个叫 None 的节点。
            CHECK(got == "节点 ? 执行失败：炸了");
            continue;
        }
        CHECK(got == c.at("error").get<std::string>());
    }
}

TEST_CASE("进度百分比的除零和 Python 一样") {
    const auto g = load_comfy_golden();
    const auto cases = g.at("progress");
    REQUIRE(cases.size() == 6);

    for (const auto& c : cases) {
        comfy::JobProgress p;
        p.prompt_id = "p1";
        p.step = c.at("step").get<int>();
        p.total = c.at("total").get<int>();
        CAPTURE(p.step);
        CAPTURE(p.total);
        CHECK(p.fraction() == doctest::Approx(c.at("fraction").get<double>()));
    }
}
