// 工作流格式转换的对拍。
//
// 语料由 tools/gen_comfy_golden.py 生成，**期望值是 Python 那边的真函数
// 算出来的**，不是手写的。手写的话对拍只能证明"C++ 和我理解的一致"。
//
// 为什么这一层值得这么重的测试：转换错了**不报错**。工作流照样提交成功、
// 照样跑完，只是每个参数都被塞进了别人的位置——出来的是一张莫名其妙的图，
// 而排查会从模型、提示词、显存一路查过去，最后才想到是参数错位。

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "comfy/workflow.hpp"
#include "util/paths.hpp"

using namespace changji;
using comfy::OrderedJson;
namespace fs = std::filesystem;

namespace {

OrderedJson load_corpus() {
    const fs::path p =
        fs::path(CHANGJI_GOLDEN_DIR) / "comfy" / "workflow_convert.json";
    std::ifstream in(p, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "语料不在：" << paths::to_utf8(p)
                                            << "，跑一遍 tools/gen_comfy_golden.py");
    std::ostringstream buf;
    buf << in.rdbuf();
    OrderedJson doc = OrderedJson::parse(buf.str(), nullptr, false);
    REQUIRE_FALSE(doc.is_discarded());
    return doc;
}

/// 第一处不一样在哪儿。直接比 dump 的话，报错只会说两个几千字的串不等。
std::string first_diff(const OrderedJson& got, const OrderedJson& want,
                       const std::string& path = "") {
    if (got == want) return {};
    if (got.type() != want.type()) {
        return path + " 类型不同：" + std::string(got.type_name()) + " vs " +
               want.type_name();
    }
    if (want.is_object()) {
        for (const auto& kv : want.items()) {
            if (!got.contains(kv.key())) return path + "/" + kv.key() + " 少了";
            const auto d = first_diff(got[kv.key()], kv.value(),
                                      path + "/" + kv.key());
            if (!d.empty()) return d;
        }
        for (const auto& kv : got.items()) {
            if (!want.contains(kv.key())) {
                return path + "/" + kv.key() + " 多出来了：" + kv.value().dump();
            }
        }
        return {};
    }
    if (want.is_array()) {
        if (got.size() != want.size()) {
            return path + " 长度不同：" + std::to_string(got.size()) + " vs " +
                   std::to_string(want.size());
        }
        for (std::size_t i = 0; i < want.size(); ++i) {
            const auto d = first_diff(got[i], want[i],
                                      path + "[" + std::to_string(i) + "]");
            if (!d.empty()) return d;
        }
        return {};
    }
    return path + " 值不同：" + got.dump() + " vs " + want.dump();
}

}  // namespace

TEST_CASE("界面版转接口版：逐个案例和 Python 对上") {
    const OrderedJson corpus = load_corpus();
    const OrderedJson& object_info = corpus.at("object_info");
    REQUIRE(corpus.at("cases").size() >= 7);

    for (const auto& c : corpus.at("cases")) {
        const std::string name = c.at("name");
        CAPTURE(name);
        // 每个案例一个新的 converter：控件名是带缓存的，共用一个的话
        // 顺序问题会被上一个案例的缓存掩盖掉。
        comfy::WorkflowConverter conv(object_info);
        const comfy::ApiWorkflow api = conv.convert(c.at("ui"));
        const std::string diff = first_diff(api.to_json(), c.at("expected"));
        CHECK_MESSAGE(diff.empty(), name << "：" << diff);
    }
}

TEST_CASE("控件名的顺序和 Python 一致") {
    // 顺序就是全部。名字取对了顺序错了，等于每个参数都填给了下一个控件。
    const OrderedJson corpus = load_corpus();
    comfy::WorkflowConverter conv(corpus.at("object_info"));
    for (const auto& kv : corpus.at("widget_names").items()) {
        CAPTURE(kv.key());
        const auto got = conv.widget_names(kv.key());
        const auto want = kv.value().get<std::vector<std::string>>();
        CHECK(got == want);
    }
}

TEST_CASE("INT 和 FLOAT 是控件不是连线") {
    // 按"全大写就是连线"判断的话，每个 KSampler 的 steps 和 cfg 都会被丢掉，
    // 而丢了之后 ComfyUI 会用默认值——图能出来，只是步数不是你填的那个。
    CHECK(comfy::is_widget_type("INT"));
    CHECK(comfy::is_widget_type("FLOAT"));
    CHECK(comfy::is_widget_type("STRING"));
    CHECK(comfy::is_widget_type("BOOLEAN"));

    CHECK_FALSE(comfy::is_widget_type("MODEL"));
    CHECK_FALSE(comfy::is_widget_type("CLIP"));
    CHECK_FALSE(comfy::is_widget_type("LATENT"));
    CHECK_FALSE(comfy::is_widget_type("CONDITIONING"));

    SUBCASE("下拉框：选项直接列在类型位置上") {
        CHECK(comfy::is_widget_type(OrderedJson::array({"euler", "uni_pc"})));
        CHECK(comfy::is_widget_type("COMBO"));
        CHECK(comfy::is_widget_type("COMFY_DYNAMICCOMBO_V3"));
    }
}

TEST_CASE("定位到多个同类节点时报错，不是随便挑一个") {
    // 一个工作流里两个 KSampler 是常见的（高噪声段和低噪声段）。
    // 随便挑一个改，出来的片子只有一半参数是对的——而那要看完整段才发现。
    comfy::ApiWorkflow w(OrderedJson{
        {"1", {{"class_type", "KSampler"}, {"inputs", OrderedJson::object()}}},
        {"2", {{"class_type", "KSampler"}, {"inputs", OrderedJson::object()}}},
        {"3", {{"class_type", "VAEDecode"}, {"inputs", OrderedJson::object()}}},
    });

    CHECK(w.find_by_class("KSampler").size() == 2);
    CHECK(w.one_by_class("VAEDecode") == "3");

    try {
        w.one_by_class("KSampler");
        FAIL("该抛");
    } catch (const comfy::WorkflowError& e) {
        const std::string msg = e.what();
        CAPTURE(msg);
        // 要把候选列出来，不然用户不知道该填哪个 id
        CHECK(msg.find("1") != std::string::npos);
        CHECK(msg.find("2") != std::string::npos);
    }

    SUBCASE("一个都没有时说的是另一句话") {
        try {
            w.one_by_class("SaveVideo");
            FAIL("该抛");
        } catch (const comfy::WorkflowError& e) {
            CHECK(std::string(e.what()).find("没有") != std::string::npos);
        }
    }
}

TEST_CASE("改参数") {
    comfy::ApiWorkflow w(OrderedJson{
        {"9", {{"class_type", "KSampler"},
               {"inputs", {{"seed", 1}, {"steps", 20}}}}},
    });

    w.set_input("9", "seed", 42);
    CHECK(w.get_input("9", "seed") == 42);

    const std::string nid = w.set_by_class("KSampler", {{"steps", 30},
                                                        {"cfg", 5.0}});
    CHECK(nid == "9");
    CHECK(w.get_input("9", "steps") == 30);
    CHECK(w.get_input("9", "cfg") == 5.0);

    SUBCASE("取不存在的返回 null，不抛") {
        // 工作流是用户给的，问一个可能不存在的键是正常操作。
        CHECK(w.get_input("9", "根本没有这个键").is_null());
        CHECK(w.get_input("没这个节点", "seed").is_null());
    }

    SUBCASE("改不存在的节点要抛") {
        // 静默成功的话，用户以为参数改上了，实际上一直用的是工作流里的原值。
        CHECK_THROWS_AS(w.set_input("404", "seed", 1), comfy::WorkflowError);
    }
}

TEST_CASE("接口版工作流直接喂进来时说清楚") {
    // 用户从 ComfyUI 导出时有两个按钮，导错那个是最常见的操作失误。
    try {
        comfy::WorkflowConverter conv(OrderedJson::object());
        conv.convert(OrderedJson{{"1", {{"class_type", "VAEDecode"}}}});
        FAIL("该抛");
    } catch (const comfy::WorkflowError& e) {
        const std::string msg = e.what();
        CAPTURE(msg);
        CHECK(msg.find("界面版") != std::string::npos);
        // 要说清怎么办
        CHECK(msg.find("ApiWorkflow") != std::string::npos);
    }
}

TEST_CASE("服务端不认识的节点类型要指向自定义节点包") {
    // 这条几乎总是"工作流是在别人的 ComfyUI 上导出的，那台装了插件"。
    // 只说"不认识"的话用户会去查工作流文件，而问题在服务端。
    comfy::WorkflowConverter conv(OrderedJson::object());
    try {
        conv.convert(OrderedJson{
            {"nodes", OrderedJson::array({
                OrderedJson{{"id", 1}, {"type", "某个插件节点"}, {"mode", 0},
                            {"widgets_values", OrderedJson::array({1})}}})},
            {"links", OrderedJson::array()},
        });
        FAIL("该抛");
    } catch (const comfy::WorkflowError& e) {
        const std::string msg = e.what();
        CAPTURE(msg);
        CHECK(msg.find("自定义节点") != std::string::npos);
    }
}

TEST_CASE("没有控件值的节点不去查控件名") {
    // 查了的话，一个纯连线的自定义节点会因为"服务端不认识"而整个工作流失败，
    // 而它其实什么参数都不用改。
    comfy::WorkflowConverter conv(OrderedJson::object());
    const auto api = conv.convert(OrderedJson{
        {"nodes", OrderedJson::array({
            OrderedJson{{"id", 1}, {"type", "谁也不认识的节点"}, {"mode", 0}}})},
        {"links", OrderedJson::array()},
    });
    CHECK(api.to_json().contains("1"));
    CHECK(api.to_json()["1"]["inputs"].empty());
}

TEST_CASE("工作流文件读不出来时带上路径") {
    try {
        comfy::load_ui_workflow("Z:/根本没有这个/workflow.json");
        FAIL("该抛");
    } catch (const comfy::WorkflowError& e) {
        CHECK(std::string(e.what()).find("workflow.json") != std::string::npos);
    }
}
