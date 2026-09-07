// 改工作流参数那一层的测试。
//
// 这里的每个判断错了都**不报错**，只是出来的东西不对：
// 正负提示词对调、尺寸改到了一个不影响输出的节点上、
// 或者参数塞进了别人的位置。而排查会从模型和提示词一路查过去。

#include <doctest/doctest.h>

#include <string>

#include "comfy/renderers.hpp"
#include "config/settings.hpp"

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
