// 引擎自己发前端那一层。
//
// 这一层没有 Python 对应物（Python 那边前端是 Node 发的，它自己只有一页
// 内置界面），所以不是对拍，是纯粹的新逻辑。
//
// 盯三件事：
//   **路径规整**——`..` 要挡住。现在文件都在内存里翻不出去，
//   但这段逻辑哪天改成读磁盘，`..` 就是目录穿越；
//   **谁接管**——`/api` 开头的绝不能被前端截走，否则整套接口全变成 HTML；
//   **Content-Type 带 charset**——界面全是中文，少了它简体 Windows 上
//   浏览器按本地代码页猜，一片乱码。

#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>

#include "http/bff_routes.hpp"
#include "http/bundled_webapp.inc.hpp"
#include "http/webapp.hpp"

using namespace changji;

TEST_CASE("请求路径规整成 dist 里的相对路径") {
    CHECK(http::normalize_webapp_path("/") == "index.html");
    CHECK(http::normalize_webapp_path("") == "index.html");
    CHECK(http::normalize_webapp_path("/index.html") == "index.html");
    CHECK(http::normalize_webapp_path("/assets/index-abc.js") ==
          "assets/index-abc.js");
    // 多个前导斜杠也该收拾干净
    CHECK(http::normalize_webapp_path("///assets/a.css") == "assets/a.css");
    // 查询串不算路径
    CHECK(http::normalize_webapp_path("/index.html?x=1") == "index.html");
    CHECK(http::normalize_webapp_path("/?a=b") == "index.html");
}

TEST_CASE("`..` 和反斜杠一律挡掉") {
    // **现在挡住，不是因为现在会出事。** 文件都在内存里，`..` 翻不出去。
    // 是因为这段规整逻辑哪天改成读磁盘，`..` 就是目录穿越，
    // 而那时候没人会想起来这里少了一道判断。
    CHECK(http::normalize_webapp_path("/../etc/passwd").empty());
    CHECK(http::normalize_webapp_path("/assets/../../secret").empty());
    CHECK(http::normalize_webapp_path("/..").empty());
    // Windows 上反斜杠同样是分隔符
    CHECK(http::normalize_webapp_path("/assets\\..\\x").empty());
    CHECK(http::normalize_webapp_path("/a\\b").empty());
}

TEST_CASE("接口路径不能被前端截走") {
    // 截走了的话整套接口全变成 HTML，而前端拿到 HTML 去 JSON.parse，
    // 报的是"意外的 <"——离真正的原因隔着十万八千里。
    CHECK_FALSE(http::webapp_owns("/api/health"));
    CHECK_FALSE(http::webapp_owns("/api/run"));
    CHECK_FALSE(http::webapp_owns("/api"));
    CHECK_FALSE(http::webapp_owns("/ws"));

    // 前端自己的路由归它管
    CHECK(http::webapp_owns("/"));
    CHECK(http::webapp_owns("/production"));
    CHECK(http::webapp_owns("/assets/index-abc.js"));
    // **前缀判断要连着斜杠**：以后真有个 /apidoc 页面，
    // 用 "/api" 裸前缀会把它误判成接口。
    CHECK(http::webapp_owns("/apidoc"));
    CHECK(http::webapp_owns("/api-guide"));
}

TEST_CASE("Content-Type 认得出常见类型，而且都带 charset") {
    CHECK(http::webapp_content_type("index.html") == "text/html; charset=utf-8");
    CHECK(http::webapp_content_type("assets/a.js") ==
          "text/javascript; charset=utf-8");
    CHECK(http::webapp_content_type("assets/a.css") == "text/css; charset=utf-8");
    CHECK(http::webapp_content_type("x.json") == "application/json; charset=utf-8");

    // 二进制的不该带 charset——带了有些代理会拿它当文本改编码
    CHECK(http::webapp_content_type("a.png") == "image/png");
    CHECK(http::webapp_content_type("a.woff2") == "font/woff2");

    // 不认识的别瞎猜
    CHECK(http::webapp_content_type("a.unknown") == "application/octet-stream");
}

TEST_CASE("前端真的嵌进来了") {
    // **这一条钉的是"打包过"这件事本身。** 忘了跑 gen_webapp.py 的话，
    // 编出来的二进制照样能起、接口照样好使，只是打开端口一片空白——
    // 比报错难查得多，因为服务是活的。
    const auto* index = http::find_webapp_file("index.html");
    REQUIRE_MESSAGE(index != nullptr,
                    "index.html 没嵌进来。跑一下："
                    "cd webapp/client && npm run build，"
                    "然后 python cpp/tools/gen_webapp.py");
    const std::string html(*index);
    CHECK(html.find("<script") != std::string::npos);
    // Vite 打出来的入口一定引 /assets/ 下的东西；没有就是空壳
    CHECK(html.find("/assets/") != std::string::npos);

    CHECK(http::find_webapp_file("不存在的文件.js") == nullptr);
}

TEST_CASE("webapp 要的每一条 /bff 接口，C++ 这边都得有") {
    // **这条用例是被一个真 bug 逼出来的。** 设置页调 /bff/settings/overview，
    // 而 C++ 那时只实现了 health / status / config / flow——那个请求 404，
    // 整页当引擎离线处理：地址空、配置文件空、右上角写"连不上"。
    // 而顶栏读的是实现了的 /bff/settings/status，显示"引擎已连接"。
    // **同一个页面上两个相反的结论**，用户先看到的是后者，于是来问
    // "为什么我这里引擎显示是连不上的"。
    //
    // 单元测试看不见这种错：两边各自都是对的，错的是**中间少了一条**。
    // 所以这里拿真正打包进去的前端代码里出现的 /bff 路径，去查 kBffRoutes。
    std::set<std::string> wanted;
    for (const auto& [name, content] : http::kBundledWebappChunks) {
        const std::string body(content);
        for (std::size_t i = body.find("/bff/"); i != std::string::npos;
             i = body.find("/bff/", i + 1)) {
            std::size_t j = i;
            while (j < body.size() &&
                   (std::isalnum(static_cast<unsigned char>(body[j])) != 0 ||
                    body[j] == '/' || body[j] == '_' || body[j] == '-')) {
                ++j;
            }
            wanted.insert(body.substr(i, j - i));
        }
    }
    REQUIRE_MESSAGE(!wanted.empty(),
                    "打包进来的前端里一条 /bff 都没找到，这个用例失效了");

    for (const auto& path : wanted) {
        CAPTURE(path);
        const bool have =
            std::find(std::begin(http::kBffRoutes), std::end(http::kBffRoutes),
                      std::string_view(path)) != std::end(http::kBffRoutes);
        CHECK(have);
    }
}