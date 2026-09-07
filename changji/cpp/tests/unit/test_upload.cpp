// 参考图上传的对拍测试。
//
// multipart 的解析不在这里测（那是 crow 的事，测它等于测第三方库）。
// 这里测的是拆完之后的部分：格式判断、大小限制、落盘、旧文件清理、
// 以及"传了图就无条件重跑全剧"这条语义。
//
// 语料里的图片是 1x1 的 PNG 和 WEBP，base64 存着。够验格式判断和落盘，
// 不需要真图片——校验逻辑只看 content_type 和字节数，不解码像素。

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "http/upload.hpp"
#include "models/project.hpp"
#include "util/paths.hpp"

using namespace changji;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

json load_golden(const std::string& name) {
    const std::string path = std::string(CHANGJI_GOLDEN_DIR) + "/" + name + ".json";
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "读不到语料 " << path);
    json j;
    in >> j;
    return j;
}

fs::path pristine_project() {
    const json exp = load_golden("project_expectations");
    return paths::from_utf8(std::string(CHANGJI_GOLDEN_DIR)) /
           paths::from_utf8(exp.at("root_name").get<std::string>());
}

fs::path fresh_copy(const std::string& tag) {
    const fs::path dst = fs::temp_directory_path() /
                         paths::from_utf8("changji_上传_" + tag);
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::copy(pristine_project(), dst, fs::copy_options::recursive, ec);
    REQUIRE_MESSAGE(!ec, "复制项目失败：" << ec.message());
    return dst;
}

/// 语料里的图片是 base64 存的。手写一个解码器，不为这点事拉个库。
std::string b64_decode(const std::string& in) {
    static const std::string kAlpha =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        const std::size_t pos = kAlpha.find(static_cast<char>(c));
        if (pos == std::string::npos) continue;  // 跳过换行之类
        val = (val << 6) + static_cast<int>(pos);
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

std::vector<std::string> list_refs(const fs::path& root) {
    std::vector<std::string> names;
    std::error_code ec;
    const fs::path refs = root / "refs";
    if (!fs::is_directory(refs, ec)) return names;
    for (const auto& e : fs::directory_iterator(refs, ec)) {
        names.push_back(paths::to_utf8(e.path().filename()));
    }
    std::sort(names.begin(), names.end());
    return names;
}

}  // namespace

TEST_CASE("参考图上传与 Python 逐条对拍") {
    const json g = load_golden("endpoints_upload");
    int idx = 0;

    for (const auto& c : g.at("cases")) {
        const std::string name = c.at("name").get<std::string>();
        const std::string url = c.at("url").get<std::string>();
        CAPTURE(name);

        const fs::path root = fresh_copy(std::to_string(idx++));
        const std::string project = paths::to_utf8(root);
        const std::string ctype = c.at("content_type").get<std::string>();
        const std::string data = b64_decode(c.at("file_b64").get<std::string>());
        const json& form = c.at("form");

        // 换格式重传那条要先传一次 png
        if (c.contains("first_png_b64")) {
            http::guard([&] {
                return http::post_character_reference(
                    project, form.at("char_id").get<std::string>(),
                    form.at("slot").get<std::string>(), "image/png",
                    b64_decode(c.at("first_png_b64").get<std::string>()));
            });
        }

        http::ApiResult got{};
        if (url == "/api/character/reference") {
            got = http::guard([&] {
                return http::post_character_reference(
                    project, form.at("char_id").get<std::string>(),
                    form.at("slot").get<std::string>(), ctype, data);
            });
        } else {
            got = http::guard([&] {
                return http::post_location_reference(
                    project, form.at("location_id").get<std::string>(),
                    ctype, data);
            });
        }

        CHECK(got.status == c.at("status").get<int>());
        if (got.body != c.at("body")) {
            MESSAGE("期望 body: " << c.at("body").dump());
            MESSAGE("实得 body: " << got.body.dump());
        }
        CHECK(got.body == c.at("body"));

        if (!c.at("assets_after").is_null()) {
            const models::ProjectStore store(root);
            const json after = store.load_assets();
            CHECK(after == c.at("assets_after"));
        }
        if (!c.at("refs_listing").is_null()) {
            CHECK(list_refs(root) ==
                  c.at("refs_listing").get<std::vector<std::string>>());
        }

        std::error_code ec;
        fs::remove_all(root, ec);
    }
}

TEST_CASE("换格式重传要把旧文件清掉") {
    // 不清的话 refs 里会留一张永远用不上的图，而且用户看不到，
    // 只有翻目录才发现。
    const fs::path root = fresh_copy("换格式");
    const std::string project = paths::to_utf8(root);
    const json g = load_golden("endpoints_upload");

    std::string png, webp;
    for (const auto& c : g.at("cases")) {
        if (c.contains("first_png_b64")) {
            png = b64_decode(c.at("first_png_b64").get<std::string>());
            webp = b64_decode(c.at("file_b64").get<std::string>());
        }
    }
    REQUIRE_FALSE(png.empty());
    REQUIRE_FALSE(webp.empty());

    http::guard([&] {
        return http::post_character_reference(project, "c_lin_yuan", "front",
                                              "image/png", png);
    });
    CHECK(list_refs(root) == std::vector<std::string>{"c_lin_yuan_front.png"});

    http::guard([&] {
        return http::post_character_reference(project, "c_lin_yuan", "front",
                                              "image/webp", webp);
    });
    // .png 必须没了
    CHECK(list_refs(root) == std::vector<std::string>{"c_lin_yuan_front.webp"});

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("超过 20MB 要被拒") {
    const fs::path root = fresh_copy("超大");
    // 只测边界逻辑，内容是什么无所谓
    const std::string big(http::kRefMaxBytes + 1, '\x89');
    const auto r = http::guard([&] {
        return http::post_character_reference(paths::to_utf8(root), "c_lin_yuan",
                                             "front", "image/png", big);
    });
    CHECK(r.status == 400);
    CHECK(r.body.at("detail").get<std::string>().find("太大了") !=
          std::string::npos);
    // 拒了就不该留下文件
    CHECK(list_refs(root).empty());

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("撤掉参考图，文件留着") {
    // 用户可能只是想先试试没有参考图的效果，删文件的话
    // 再想用回来就得重新找那张图。
    const fs::path root = fresh_copy("撤图");
    const std::string project = paths::to_utf8(root);
    const json g = load_golden("endpoints_upload");

    std::string png;
    for (const auto& c : g.at("cases")) {
        if (c.at("content_type").get<std::string>() == "image/png" &&
            c.at("status").get<int>() == 200) {
            png = b64_decode(c.at("file_b64").get<std::string>());
            break;
        }
    }
    REQUIRE_FALSE(png.empty());

    http::guard([&] {
        return http::post_character_reference(project, "c_lin_yuan", "front",
                                              "image/png", png);
    });
    REQUIRE(list_refs(root).size() == 1);

    const json body = {{"project", project}, {"char_id", "c_lin_yuan"},
                       {"slot", "front"}};
    const auto r = http::guard([&] {
        return http::post_character_reference_clear(body);
    });
    REQUIRE(r.status == 200);
    CHECK(r.body.at("cleared").get<bool>() == true);

    // 字段清了
    const models::ProjectStore store(root);
    CHECK_FALSE(store.load_assets().characters.at("c_lin_yuan").ref_front.has_value());
    // 文件还在
    CHECK(list_refs(root).size() == 1);

    SUBCASE("再撤一次是 cleared:false，不报错") {
        const auto r2 = http::guard([&] {
            return http::post_character_reference_clear(body);
        });
        CHECK(r2.status == 200);
        CHECK(r2.body.at("cleared").get<bool>() == false);
        CHECK(r2.body.at("reset_shots").get<int>() == 0);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("格式判断") {
    CHECK(http::ref_suffix_for("image/png") == ".png");
    CHECK(http::ref_suffix_for("image/jpeg") == ".jpg");
    CHECK(http::ref_suffix_for("image/webp") == ".webp");
    // 这几个都不收
    CHECK(http::ref_suffix_for("image/gif").empty());
    CHECK(http::ref_suffix_for("image/jpg").empty());   // 注意不是 image/jpeg
    CHECK(http::ref_suffix_for("application/octet-stream").empty());
    CHECK(http::ref_suffix_for("").empty());
}
