#pragma once

// GET /api/media —— 取项目内的文件。
//
// 两件事 Python 那边是白拿的、这里必须自己写：
//
// 1. **越界检查**。Starlette 没做，是路由里手写的 relative_to 判断；
//    这里同样手写，但要用 weakly_canonical 处理 .. 和符号链接。
//
// 2. **HTTP Range**。Python 用 FastAPI 的 FileResponse，Starlette 自带
//    206 Partial Content。Crow 没有——不实现的话前端 <video> 标签
//    **拖不动进度条**，只能从头播。方案第三节把这一条单独点了名。

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace changji::http {

/// 把 rel 解析成项目内的绝对路径。
///
/// 分开成纯函数是为了能单测越界检查——那是安全边界，
/// 不该只在起了服务之后才验证。
struct MediaTarget {
    int status = 200;             ///< 200 表示解析成功
    std::string detail;           ///< 出错时的说明
    std::filesystem::path path;   ///< 解析出来的绝对路径
};

MediaTarget resolve_media(const std::string& project_path, const std::string& rel);

/// 解析 Range 请求头。
///
/// 只支持单区间的 `bytes=start-end`，这是浏览器 <video> 唯一会发的形式。
/// 多区间（`bytes=0-99,200-299`）返回 nullopt，调用方按整文件回。
struct ByteRange {
    std::uint64_t first = 0;
    std::uint64_t last = 0;  ///< 闭区间，含 last
};

/// file_size 用来把开放式区间（`bytes=100-`、`bytes=-500`）算成具体范围。
/// 语法不认识或者范围越界返回 nullopt。
std::optional<ByteRange> parse_range(const std::string& header,
                                     std::uint64_t file_size);

/// 按扩展名猜 Content-Type。
///
/// 不装 libmagic：项目里只会出现 mp4/png/jpg/webp/wav 这几种，
/// 而且猜错的后果是浏览器不播，不是安全问题。
std::string content_type_for(const std::filesystem::path& p);

}  // namespace changji::http
