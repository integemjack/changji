#include "http/media.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "models/project.hpp"
#include "util/paths.hpp"

namespace fs = std::filesystem;

namespace changji::http {

MediaTarget resolve_media(const std::string& project_path, const std::string& rel) {
    MediaTarget out;
    if (project_path.empty()) {
        return {400, "没有指定项目目录", {}};
    }

    const models::ProjectPaths paths(paths::from_utf8(project_path));
    std::error_code ec;

    // 先拼再规范化。weakly_canonical 会把 .. 和符号链接都解开，
    // 这是越界检查能成立的前提——只做字符串前缀比较的话，
    // rel 传 "../../etc/passwd" 就穿出去了。
    const fs::path target = fs::weakly_canonical(
        paths.root() / changji::paths::from_utf8(rel), ec);
    const fs::path root = fs::weakly_canonical(paths.root(), ec);

    // 判断 target 是不是在 root 之下。用 lexically_relative 而不是
    // 字符串前缀：后者会把 /a/bc 误判成在 /a/b 之下。
    const fs::path relative = target.lexically_relative(root);
    if (relative.empty() || *relative.begin() == "..") {
        return {403, "只能访问项目目录内的文件", {}};
    }

    if (!fs::is_regular_file(target, ec)) {
        return {404, "文件不存在", {}};
    }

    out.path = target;
    return out;
}

std::optional<ByteRange> parse_range(const std::string& header,
                                     std::uint64_t file_size) {
    if (file_size == 0) return std::nullopt;

    // 只认 "bytes=" 开头
    const std::string prefix = "bytes=";
    if (header.compare(0, prefix.size(), prefix) != 0) return std::nullopt;
    std::string spec = header.substr(prefix.size());

    // 多区间不支持，按整文件回。浏览器的 <video> 不会发这种。
    if (spec.find(',') != std::string::npos) return std::nullopt;

    const std::size_t dash = spec.find('-');
    if (dash == std::string::npos) return std::nullopt;

    const std::string first_s = spec.substr(0, dash);
    const std::string last_s = spec.substr(dash + 1);

    const auto all_digits = [](const std::string& s) {
        return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
        });
    };

    ByteRange r;
    if (first_s.empty()) {
        // "bytes=-500" —— 最后 500 字节
        if (!all_digits(last_s)) return std::nullopt;
        const std::uint64_t n = std::strtoull(last_s.c_str(), nullptr, 10);
        if (n == 0) return std::nullopt;
        r.first = n >= file_size ? 0 : file_size - n;
        r.last = file_size - 1;
    } else {
        if (!all_digits(first_s)) return std::nullopt;
        r.first = std::strtoull(first_s.c_str(), nullptr, 10);
        if (last_s.empty()) {
            // "bytes=100-" —— 从 100 到末尾。播放器拖进度条发的就是这种。
            r.last = file_size - 1;
        } else {
            if (!all_digits(last_s)) return std::nullopt;
            r.last = std::strtoull(last_s.c_str(), nullptr, 10);
        }
    }

    if (r.first > r.last) return std::nullopt;
    if (r.first >= file_size) return std::nullopt;  // 起点越界，调用方应回 416
    if (r.last >= file_size) r.last = file_size - 1;
    return r;
}

std::string content_type_for(const fs::path& p) {
    std::string ext = paths::to_utf8(p.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (ext == ".mp4")  return "video/mp4";
    if (ext == ".avi")  return "video/x-msvideo";
    if (ext == ".webm") return "video/webm";
    if (ext == ".png")  return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".webp") return "image/webp";
    if (ext == ".wav")  return "audio/wav";
    if (ext == ".mp3")  return "audio/mpeg";
    if (ext == ".srt")  return "application/x-subrip";
    if (ext == ".ass")  return "text/x-ssa";
    if (ext == ".json") return "application/json";
    if (ext == ".txt")  return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

}  // namespace changji::http
