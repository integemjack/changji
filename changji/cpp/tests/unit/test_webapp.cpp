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
#include <map>
#include <string>

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

TEST_CASE("切成多段的文件要拼回完整长度") {
    // **原来这里只查了几个子串**——而子串在第一段里就有，
    // 后面几段丢了照样绿。实机上正是这样：浏览器拿到 65389 字节
    // 而磁盘上是 131332，页面一片空白，而这条用例是绿的。
    //
    // 生成器把大文件切成 16 KB 一段（MSVC 单条字面量的上限），
    // 运行时按文件名拼回去。**长度是唯一能证明拼对了的东西。**
    std::size_t total = 0;
    std::size_t biggest = 0;
    for (const auto& [name, chunk] : http::kBundledWebappChunks) {
        (void)name;
        total += chunk.size();
    }
    // 逐个文件查：拼出来的长度必须等于它那几段之和
    std::map<std::string, std::size_t> want;
    for (const auto& [name, chunk] : http::kBundledWebappChunks) {
        want[std::string(name)] += chunk.size();
    }
    std::size_t joined = 0;
    for (const auto& [name, size] : want) {
        const auto* got = http::find_webapp_file(name);
        REQUIRE_MESSAGE(got != nullptr, "少了 " << name);
        CHECK_MESSAGE(got->size() == size,
                      name << " 拼出来 " << got->size() << " 字节，"
                           << "该是 " << size);
        joined += got->size();
        biggest = std::max(biggest, size);
    }
    CHECK(joined == total);
    // 确认语料里真有需要切段的文件，否则这条用例什么都没验
    CHECK_MESSAGE(biggest > 16000,
                  "最大的文件才 " << biggest << " 字节，没有跨段的，"
                                  "这条用例证明不了拼接是对的");
}
