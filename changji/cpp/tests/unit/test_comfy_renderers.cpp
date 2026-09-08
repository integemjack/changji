// 改工作流参数那一层的测试。
//
// 这里的每个判断错了都**不报错**，只是出来的东西不对：
// 正负提示词对调、尺寸改到了一个不影响输出的节点上、
// 或者参数塞进了别人的位置。而排查会从模型和提示词一路查过去。

#include <doctest/doctest.h>

#include <string>

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <vector>

#include "comfy/renderers.hpp"
#include "config/settings.hpp"
#include "stages/prompt_compose.hpp"
#include "util/paths.hpp"

using namespace changji;
using comfy::OrderedJson;

namespace {

/// 一个最小的可用工作流：KSampler 接了正负两个文本节点。
comfy::ApiWorkflow sampler_workflow(const std::string& pos_id,
                                    const std::string& neg_id) {
    return comfy::ApiWorkflow(OrderedJson{
        {pos_id, {{"class_type", "CLIPTextEncode"}, {"inputs", {{"text", "正"}}}}},
        {neg_id, {{"class_type", "CLIPTextEncode"}, {"inputs", {{"text", "负"}}}}},
        {"9", {{"class_type", "KSampler"}, {"inputs", {
            {"positive", OrderedJson::array({pos_id, 0})},
            {"negative", OrderedJson::array({neg_id, 0})},
            {"steps", 20}}}}},
    });
}

namespace fs = std::filesystem;

/// 一台够用的假 ComfyUI：只记不判。
///
/// 不走 WebSocket（connect_ws 返回空），让 wait() 退回查历史那条路——
/// 这里要测的是"改了工作流的哪些地方"，等待逻辑在 test_comfy_client 里。
struct Recorder {
    std::vector<OrderedJson> submitted;      ///< 每次提交的 prompt
    std::vector<fs::path> uploaded;          ///< upload_image 收到的本地路径
    std::vector<fs::path> downloaded_to;     ///< download 写到哪儿
    int post_status = 200;
    std::string post_body = R"({"prompt_id":"pid-1"})";
    /// 产出为空时上层该抛"没有产出"
    bool empty_outputs = false;

    comfy::Transport transport() {
        comfy::Transport t;
        t.get = [this](const std::string& path, double) {
            if (path.rfind("/history/", 0) != 0) return resp(200, "{}");
            const OrderedJson outputs =
                empty_outputs
                    ? OrderedJson::object()
                    : OrderedJson{{"12", {{"images", OrderedJson::array({
                          OrderedJson{{"filename", "out.png"},
                                      {"subfolder", ""},
                                      {"type", "output"}}})}}}};
            return resp(200, OrderedJson{{"pid-1", {
                {"status", {{"status_str", "success"}, {"completed", true}}},
                {"outputs", outputs}}}}.dump());
        };
        t.post_json = [this](const std::string&, const std::string& body, double) {
            submitted.push_back(OrderedJson::parse(body).at("prompt"));
            return resp(post_status, post_body);
        };
        t.upload = [this](const std::string&, const fs::path& p,
                          const std::string&, double) {
            uploaded.push_back(p);
            return resp(200, OrderedJson{
                {"name", "up_" + std::to_string(uploaded.size()) + ".png"},
                {"subfolder", ""}}.dump());
        };
        t.download = [this](const std::string&,
                            const std::map<std::string, std::string>&,
                            const fs::path& dest, double) {
            downloaded_to.push_back(dest);
            std::ofstream f(dest, std::ios::binary);
            f << "假的产出";
            return resp(200, "");
        };
        t.connect_ws = [](const std::string&) -> comfy::WsRecv { return nullptr; };
        t.sleep = [](double) {};
        return t;
    }

    /// 最后一次提交里某个节点的某个输入。
    OrderedJson input(const std::string& node, const std::string& key) const {
        REQUIRE_FALSE(submitted.empty());
        return submitted.back().at(node).at("inputs").at(key);
    }

    /// 最后一次提交里第一个该类节点的某个输入。
    OrderedJson by_class(const std::string& cls, const std::string& key) const {
        REQUIRE_FALSE(submitted.empty());
        for (const auto& [id, node] : submitted.back().items()) {
            if (node.at("class_type") == cls) return node.at("inputs").at(key);
        }
        FAIL("提交里没有 " << cls << " 节点");
        return {};
    }

private:
    static comfy::HttpResponse resp(int code, const std::string& body) {
        return {code, body, std::nullopt};
    }
};

comfy::ConfigProvider quick_config() {
    return [] {
        config::ComfyConfig c;
        c.job_timeout_s = 5.0;
        c.max_retries = 0;
        return c;
    };
}

std::shared_ptr<comfy::Client> client_of(Recorder& r) {
    return std::make_shared<comfy::Client>(quick_config(), r.transport(), "cid");
}

models::Shot make_shot(const std::string& id = "ep01_sh007") {
    models::Shot s;
    s.shot_id = id;
    s.scene_id = "sc01";
    s.visual_desc = "雨夜天台";
    s.first_frame_prompt = "雨夜天台，两人对峙";
    s.motion_prompt = "镜头缓慢推近";
    s.duration_s = 5.0;
    return s;
}

models::TierSpec make_spec(models::Tier tier = models::Tier::DRAFT) {
    models::TierSpec s;
    s.tier = tier;
    s.width = 480;
    s.height = 854;
    s.steps = 4;
    return s;
}

fs::path temp_dir(const std::string& tag) {
    const fs::path d = fs::temp_directory_path() /
                       paths::from_utf8("changji_渲染_" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

}  // namespace

TEST_CASE("正负提示词靠连线认，不靠节点 id 大小猜") {
    // id 的大小只是画布上的创建顺序，和它接到采样器的哪个输入槽没有关系。
    // 猜错的后果是正负提示词对调——画面里出现的全是负面词里写的东西，
    // 而且不会报任何错。
    SUBCASE("正的 id 比负的小") {
        const auto [p, n] = comfy::text_node_ids(sampler_workflow("5", "6"));
        CHECK(p == "5");
        CHECK(n == "6");
    }
    SUBCASE("正的 id 比负的大——按 id 猜就会在这里反过来") {
        const auto [p, n] = comfy::text_node_ids(sampler_workflow("6", "5"));
        CHECK(p == "6");
        CHECK(n == "5");
    }
    SUBCASE("id 是数字类型的连线也要认") {
        // 老版本导出的接口版工作流里源节点 id 是整数不是字符串。
        comfy::ApiWorkflow w(OrderedJson{
            {"9", {{"class_type", "KSampler"}, {"inputs", {
                {"positive", OrderedJson::array({5, 0})},
                {"negative", OrderedJson::array({6, 0})}}}}},
        });
        const auto [p, n] = comfy::text_node_ids(w);
        CHECK(p == "5");
        CHECK(n == "6");
    }
}

TEST_CASE("采样器没接正负提示词时报错，不是挑一个") {
    // 挑一个的话，提示词会写进一个不影响输出的节点，
    // 表现是"改了提示词画面纹丝不动"。
    comfy::ApiWorkflow w(OrderedJson{
        {"9", {{"class_type", "KSampler"}, {"inputs", {{"steps", 20}}}}},
    });
    try {
        comfy::text_node_ids(w);
        FAIL("该抛");
    } catch (const comfy::WorkflowError& e) {
        CHECK(std::string(e.what()).find("正负提示词") != std::string::npos);
    }

    SUBCASE("接的是常量而不是连线也算没接") {
        comfy::ApiWorkflow c(OrderedJson{
            {"9", {{"class_type", "KSampler"}, {"inputs", {
                {"positive", "一个字符串"},
                {"negative", "另一个字符串"}}}}},
        });
        CHECK_THROWS_AS(comfy::text_node_ids(c), comfy::WorkflowError);
    }
}

TEST_CASE("尺寸节点逐个试，顺序有讲究") {
    SUBCASE("视频那个优先") {
        // 一个工作流里同时有视频潜变量节点和空潜变量节点时，
        // 改错那个不影响输出——分辨率设置看着生效了，实际没有。
        comfy::ApiWorkflow w(OrderedJson{
            {"1", {{"class_type", "EmptyLatentImage"},
                   {"inputs", {{"width", 512}, {"height", 512}}}}},
            {"8", {{"class_type", "Wan22ImageToVideoLatent"},
                   {"inputs", {{"width", 512}, {"height", 512}}}}},
        });
        CHECK(comfy::set_size(w, 640, 352));
        CHECK(w.get_input("8", "width") == 640);
        CHECK(w.get_input("1", "width") == 512);   // 没被动
    }

    SUBCASE("只有空潜变量节点时也能改") {
        comfy::ApiWorkflow w(OrderedJson{
            {"1", {{"class_type", "EmptySD3LatentImage"},
                   {"inputs", {{"width", 512}, {"height", 512}}}}},
        });
        CHECK(comfy::set_size(w, 448, 768));
        CHECK(w.get_input("1", "width") == 448);
        CHECK(w.get_input("1", "height") == 768);
    }

    SUBCASE("一个都没有时返回 false，不抛") {
        // 有些工作流的尺寸是写死在别处的，那也能出片，只是不听档位的。
        // 抛的话这类工作流整个用不了。
        comfy::ApiWorkflow w(OrderedJson{
            {"9", {{"class_type", "KSampler"}, {"inputs", {{"steps", 20}}}}},
        });
        CHECK_FALSE(comfy::set_size(w, 640, 352));
    }
}

TEST_CASE("engine 只能是 sd 或 comfy") {
    // 它是枚举，写错了不是"文件缺了"而是"整条出片的路走岔了"。
    // 走岔的表现是连不上 ComfyUI 或者报"没有编进出图后端"——
    // 两句话都指不到真正的原因（拼写错误）。
    config::ModelsConfig m;
    CHECK(m.engine == "sd");          // 默认是进程内那条路
    CHECK(m.validate().empty());

    m.engine = "comfy";
    CHECK(m.validate().empty());

    m.engine = "ComfyUI";
    const auto errs = m.validate();
    REQUIRE(errs.size() == 1);
    CAPTURE(errs[0]);
    CHECK(errs[0].find("ComfyUI") != std::string::npos);   // 把填错的值回显出来
    CHECK(errs[0].find("sd") != std::string::npos);        // 和可选值

    SUBCASE("模型文件仍然一个都不查") {
        // 装好程序还没下模型是常态。那时候如果配置加载直接失败，
        // 用户连界面都进不去，也就没法在界面里看到缺哪个文件。
        config::ModelsConfig empty;
        CHECK(empty.video.empty());
        CHECK(empty.validate().empty());
    }
}

// ===========================================================================
// 两个后端函数对象本身。**它们原来一条测试都没有**（覆盖率脚本报的
// image_frame_renderer / video_renderer），而这是 ComfyUI 那条路真正
// 出图出片的地方——上面那些用例只测了改工作流的三个小工具。
// ===========================================================================

namespace {

/// 出首帧的工作流：采样器 + 尺寸节点 + 保存节点 + 两个图片加载节点。
comfy::ApiWorkflow image_workflow() {
    return comfy::ApiWorkflow(OrderedJson{
        {"3", {{"class_type", "CLIPTextEncode"}, {"inputs", {{"text", ""}}}}},
        {"4", {{"class_type", "CLIPTextEncode"}, {"inputs", {{"text", ""}}}}},
        {"5", {{"class_type", "EmptyLatentImage"},
               {"inputs", {{"width", 512}, {"height", 512}}}}},
        {"6", {{"class_type", "LoadImage"}, {"inputs", {{"image", ""}}}}},
        {"7", {{"class_type", "LoadImage"}, {"inputs", {{"image", ""}}}}},
        {"8", {{"class_type", "SaveImage"},
               {"inputs", {{"filename_prefix", "ComfyUI"}}}}},
        {"9", {{"class_type", "KSampler"}, {"inputs", {
            {"positive", OrderedJson::array({"3", 0})},
            {"negative", OrderedJson::array({"4", 0})},
            {"seed", 0}, {"steps", 20}}}}},
    });
}

comfy::ApiWorkflow video_workflow() {
    return comfy::ApiWorkflow(OrderedJson{
        {"3", {{"class_type", "CLIPTextEncode"}, {"inputs", {{"text", ""}}}}},
        {"4", {{"class_type", "CLIPTextEncode"}, {"inputs", {{"text", ""}}}}},
        {"5", {{"class_type", "Wan22ImageToVideoLatent"},
               {"inputs", {{"width", 0}, {"height", 0}, {"length", 0}}}}},
        {"6", {{"class_type", "LoadImage"}, {"inputs", {{"image", ""}}}}},
        {"8", {{"class_type", "SaveVideo"},
               {"inputs", {{"filename_prefix", "ComfyUI"}}}}},
        {"9", {{"class_type", "KSampler"}, {"inputs", {
            {"positive", OrderedJson::array({"3", 0})},
            {"negative", OrderedJson::array({"4", 0})},
            {"seed", 0}, {"steps", 20}}}}},
    });
}

stages::PromptBundle bundle(std::vector<std::string> refs = {}) {
    stages::PromptBundle b;
    b.positive = "雨夜天台，两人对峙";
    b.negative = "低质量，多余的手";
    b.reference_images = std::move(refs);
    return b;
}

void touch(const fs::path& p) {
    std::ofstream f(p, std::ios::binary);
    f << "png";
}

}  // namespace

TEST_CASE("出首帧：提示词进对节点，不是按 id 排的") {
    // **正负对调不报任何错**，画面里出现的全是负面词写的东西。
    // 这个工作流里正的是 3、负的是 4，但连线才是判据——
    // 下面那个 SUBCASE 把连线反过来，按 id 猜就会在那里翻车。
    Recorder rec;
    const auto dir = temp_dir("首帧");
    models::ProjectPaths paths(dir);
    auto render = comfy::image_frame_renderer(client_of(rec), image_workflow(),
                                              paths);
    const auto shot = make_shot();
    pipeline::CancelToken tok;
    render(shot, bundle(), make_spec(), dir / "f.png", tok,
           [](int, int, double) {});

    CHECK(rec.input("3", "text") == "雨夜天台，两人对峙");
    CHECK(rec.input("4", "text") == "低质量，多余的手");

    SUBCASE("连线反过来，提示词也跟着换节点") {
        Recorder r2;
        auto wf = image_workflow();
        wf.set_input("9", "positive", OrderedJson::array({"4", 0}));
        wf.set_input("9", "negative", OrderedJson::array({"3", 0}));
        auto r = comfy::image_frame_renderer(client_of(r2), wf, paths);
        r(shot, bundle(), make_spec(), dir / "g.png", tok,
          [](int, int, double) {});
        CHECK(r2.input("4", "text") == "雨夜天台，两人对峙");
        CHECK(r2.input("3", "text") == "低质量，多余的手");
    }
}

TEST_CASE("出首帧：尺寸、种子、保存前缀") {
    Recorder rec;
    const auto dir = temp_dir("首帧参数");
    models::ProjectPaths paths(dir);
    auto render = comfy::image_frame_renderer(client_of(rec), image_workflow(),
                                              paths);
    auto shot = make_shot();
    pipeline::CancelToken tok;
    render(shot, bundle(), make_spec(), dir / "f.png", tok,
           [](int, int, double) {});

    CHECK(rec.input("5", "width") == 480);
    CHECK(rec.input("5", "height") == 854);

    // 种子必须是 frame_seed 算出来的：随机种子的话同一个镜头重跑两次
    // 出来的画面不一样，而"重跑一次看看"是用户最常做的操作。
    CHECK(rec.input("9", "seed") == stages::frame_seed(shot.shot_id, 0));

    // 前缀带上 shot_id，产出目录里才认得出哪张是哪一镜。
    CHECK(rec.input("8", "filename_prefix") == "changji/frame_ep01_sh007");

    SUBCASE("重试要换种子，否则重跑出来的是同一张") {
        Recorder r2;
        auto s2 = shot;
        s2.attempts = 1;
        auto r = comfy::image_frame_renderer(client_of(r2), image_workflow(),
                                             paths);
        r(s2, bundle(), make_spec(), dir / "g.png", tok,
          [](int, int, double) {});
        CHECK(r2.input("9", "seed") == stages::frame_seed(shot.shot_id, 1));
        CHECK(r2.input("9", "seed") != rec.input("9", "seed"));
    }

    SUBCASE("工作流没有 SaveImage 也要能跑完") {
        // 有些工作流用别的保存节点。set_by_class 会抛 WorkflowError，
        // 那个异常是被吞掉的——**吞掉是对的**，用默认前缀照样取得到产出。
        Recorder r2;
        auto wf = image_workflow();
        wf.to_json().erase("8");
        auto r = comfy::image_frame_renderer(client_of(r2), wf, paths);
        CHECK_NOTHROW(r(shot, bundle(), make_spec(), dir / "h.png", tok,
                        [](int, int, double) {}));
    }
}

TEST_CASE("出首帧：参考图要先传上去，ComfyUI 可能在别的机器上") {
    Recorder rec;
    const auto dir = temp_dir("参考图");
    models::ProjectPaths paths(dir);
    fs::create_directories(dir / "assets");
    touch(dir / "assets" / "a.png");
    touch(dir / "assets" / "b.png");

    auto render = comfy::image_frame_renderer(client_of(rec), image_workflow(),
                                              paths);
    pipeline::CancelToken tok;
    render(make_shot(), bundle({"assets/a.png", "assets/b.png"}), make_spec(),
           dir / "f.png", tok, [](int, int, double) {});

    REQUIRE(rec.uploaded.size() == 2);
    // 传的是绝对路径（paths.abs 解出来的），不是项目内的相对路径——
    // 相对路径在另一台机器上没有意义。
    CHECK(rec.uploaded[0].is_absolute());
    // 两个 LoadImage 按顺序填
    CHECK(rec.input("6", "image") == "up_1.png");
    CHECK(rec.input("7", "image") == "up_2.png");

    SUBCASE("参考图比 LoadImage 多，多出来的丢掉而不是崩") {
        // 工作流能吃几张是工作流的事，这一层不该去改它的结构。
        Recorder r2;
        touch(dir / "assets" / "c.png");
        auto r = comfy::image_frame_renderer(client_of(r2), image_workflow(),
                                             paths);
        CHECK_NOTHROW(r(make_shot(),
                        bundle({"assets/a.png", "assets/b.png", "assets/c.png"}),
                        make_spec(), dir / "g.png", tok,
                        [](int, int, double) {}));
        CHECK(r2.uploaded.size() == 3);      // 三张都传了
        CHECK(r2.input("6", "image") == "up_1.png");
        CHECK(r2.input("7", "image") == "up_2.png");
    }

    SUBCASE("参考图文件不在了，跳过它而不是让整镜失败") {
        // 资产被手工删掉、或者项目从别的机器拷过来时断链。
        // 为这个让整镜挂掉的话，一张丢失的定妆图会卡住整集。
        Recorder r2;
        auto r = comfy::image_frame_renderer(client_of(r2), image_workflow(),
                                             paths);
        CHECK_NOTHROW(r(make_shot(),
                        bundle({"assets/没这个.png", "assets/b.png"}),
                        make_spec(), dir / "h.png", tok,
                        [](int, int, double) {}));
        REQUIRE(r2.uploaded.size() == 1);
        CHECK(r2.input("6", "image") == "up_1.png");
    }
}

TEST_CASE("出首帧：产出下载到 dest，没有产出就报到镜头号") {
    Recorder rec;
    const auto dir = temp_dir("首帧产出");
    models::ProjectPaths paths(dir);
    const fs::path dest = dir / "frame.png";
    auto render = comfy::image_frame_renderer(client_of(rec), image_workflow(),
                                              paths);
    pipeline::CancelToken tok;
    render(make_shot(), bundle(), make_spec(), dest, tok,
           [](int, int, double) {});

    REQUIRE(rec.downloaded_to.size() == 1);
    CHECK(rec.downloaded_to[0] == dest);
    CHECK(fs::exists(dest));

    SUBCASE("跑完了但没有产出") {
        // "跑成功了但什么都没出来"要说清楚，否则上层只看到 dest 不存在。
        Recorder r2;
        r2.empty_outputs = true;
        auto r = comfy::image_frame_renderer(client_of(r2), image_workflow(),
                                             paths);
        CHECK_THROWS_AS(r(make_shot(), bundle(), make_spec(), dest, tok,
                          [](int, int, double) {}),
                        comfy::ComfyError);
    }
}

TEST_CASE("出视频：帧数、步数、种子、档位前缀") {
    Recorder rec;
    const auto dir = temp_dir("视频参数");
    auto render = comfy::video_renderer(client_of(rec), video_workflow());

    const models::AssetLibrary a;
    const stages::PromptComposer composer(a);
    auto shot = make_shot();
    const auto plan = stages::make_plan(shot, make_spec(models::Tier::FINAL),
                                        composer, "9:16");
    pipeline::CancelToken tok;
    render(shot, plan, std::nullopt, dir / "v.mp4", tok,
           [](int, int, double) {});

    CHECK(rec.input("5", "width") == plan.spec.width);
    CHECK(rec.input("5", "height") == plan.spec.height);
    CHECK(rec.input("5", "length") == plan.frames);
    CHECK(rec.input("9", "steps") == plan.spec.steps);
    CHECK(rec.input("9", "seed") == stages::render_seed(shot.shot_id, 0));

    // 档位进文件名：草稿和成片出在同一个目录里，前缀一样就会互相覆盖。
    CHECK(rec.input("8", "filename_prefix") == "changji/ep01_sh007_final");

    SUBCASE("草稿档的前缀不一样") {
        Recorder r2;
        const auto dplan = stages::make_plan(shot, make_spec(models::Tier::DRAFT),
                                             composer, "9:16");
        auto r = comfy::video_renderer(client_of(r2), video_workflow());
        r(shot, dplan, std::nullopt, dir / "d.mp4", tok, [](int, int, double) {});
        CHECK(r2.input("8", "filename_prefix") == "changji/ep01_sh007_draft");
    }
}

TEST_CASE("出视频：首帧传上去，没有首帧就不传") {
    Recorder rec;
    const auto dir = temp_dir("视频首帧");
    touch(dir / "first.png");

    const models::AssetLibrary a;
    const stages::PromptComposer composer(a);
    const auto shot = make_shot();
    const auto plan = stages::make_plan(shot, make_spec(), composer, "9:16");
    pipeline::CancelToken tok;

    auto render = comfy::video_renderer(client_of(rec), video_workflow());
    render(shot, plan, dir / "first.png", dir / "v.mp4", tok,
           [](int, int, double) {});
    REQUIRE(rec.uploaded.size() == 1);
    CHECK(rec.input("6", "image") == "up_1.png");

    SUBCASE("没有首帧时不碰 LoadImage") {
        // 文生视频那条路没有首帧。硬塞一个空文件名进去的话，
        // ComfyUI 会报"找不到文件"，而那句话指不到这里。
        Recorder r2;
        auto r = comfy::video_renderer(client_of(r2), video_workflow());
        r(shot, plan, std::nullopt, dir / "w.mp4", tok, [](int, int, double) {});
        CHECK(r2.uploaded.empty());
        CHECK(r2.input("6", "image") == "");
    }
}

TEST_CASE("出视频：正向提示词跟着风格线走") {
    // **这是那个 bug 的现场。** 两个视频后端原来都写死了 REALISTIC，
    // 动漫线的项目拼出来是"画面，运动"，而 Python 是"画面, 运动"。
    // test_render / test_prompt_compose 测的是 video_positive 本身，
    // 这一条测的是**后端有没有把风格线带进去**——bug 就在这一层。
    const auto dir = temp_dir("视频风格线");
    pipeline::CancelToken tok;
    const auto shot = make_shot();

    auto positive_for = [&](models::StyleLine line) {
        models::AssetLibrary a;
        a.style.style_line = line;
        const stages::PromptComposer composer(a);
        const auto plan = stages::make_plan(shot, make_spec(), composer, "9:16");
        REQUIRE_FALSE(plan.motion.empty());
        Recorder rec;
        auto r = comfy::video_renderer(client_of(rec), video_workflow());
        r(shot, plan, std::nullopt, dir / "v.mp4", tok, [](int, int, double) {});
        return rec.input("3", "text").get<std::string>();
    };

    const std::string anime = positive_for(models::StyleLine::ANIME);
    const std::string real = positive_for(models::StyleLine::REALISTIC);

    CHECK(anime.find(", ") != std::string::npos);
    CHECK(anime != real);
    CHECK(real.find("，") != std::string::npos);
}

TEST_CASE("被服务端拒绝时，报的话里有镜头号，不是一坨嵌套 JSON") {
    // 400 是校验失败（比如工作流里写了个不存在的模型文件名）。
    // 原样透传的话用户看到的是 ComfyUI 的 node_errors 结构，
    // 而那里面既没有镜头号也没有人话。
    const auto dir = temp_dir("拒绝");
    models::ProjectPaths paths(dir);
    pipeline::CancelToken tok;

    Recorder rec;
    rec.post_status = 400;
    rec.post_body = OrderedJson{
        {"error", {{"type", "prompt_outputs_failed_validation"},
                   {"message", "Prompt outputs failed validation"}}},
        {"node_errors", {{"4", {
            {"class_type", "CheckpointLoaderSimple"},
            {"errors", OrderedJson::array({
                OrderedJson{{"message", "Value not in list"},
                            {"extra_info", {{"input_name", "ckpt_name"},
                                            {"received_value",
                                             "nope.safetensors"}}}}})}}}}},
    }.dump();

    auto render = comfy::image_frame_renderer(client_of(rec), image_workflow(),
                                              paths);
    std::string why;
    try {
        render(make_shot(), bundle(), make_spec(), dir / "f.png", tok,
               [](int, int, double) {});
        FAIL("应该抛");
    } catch (const comfy::ComfyError& e) {
        why = e.what();
    }
    CHECK(why.find("ep01_sh007") != std::string::npos);
    // human_summary 的内容要在，否则用户不知道是哪个模型名写错了。
    // 它读的是 extra_info.input_name / received_value——和 Python 的
    // human_summary 逐字段一致（对过 src/changji/comfy/client.py:50）。
    CHECK(why.find("nope.safetensors") != std::string::npos);
    CHECK(why.find("ckpt_name") != std::string::npos);
    // 而不是把 node_errors 原样倒出来
    CHECK(why.find("extra_info") == std::string::npos);

    SUBCASE("视频那条路也一样") {
        Recorder r2;
        r2.post_status = rec.post_status;
        r2.post_body = rec.post_body;
        const models::AssetLibrary a;
        const stages::PromptComposer composer(a);
        const auto plan = stages::make_plan(make_shot(), make_spec(),
                                            composer, "9:16");
        auto r = comfy::video_renderer(client_of(r2), video_workflow());
        CHECK_THROWS_AS(r(make_shot(), plan, std::nullopt, dir / "v.mp4", tok,
                          [](int, int, double) {}),
                        comfy::ComfyError);
    }
}
