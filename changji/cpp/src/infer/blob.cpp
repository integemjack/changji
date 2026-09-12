#include "infer/blob.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include "util/paths.hpp"
#include "util/text.hpp"

namespace changji::infer {

namespace fs = std::filesystem;

namespace {

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        throw std::runtime_error("读不了：" + paths::to_utf8(p));
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

bool blob_id_ok(const std::string& id) {
    if (id.size() != 40) return false;
    for (const char c : id) {
        const bool digit = c >= '0' && c <= '9';
        const bool hex = c >= 'a' && c <= 'f';
        if (!digit && !hex) return false;
    }
    return true;
}

fs::path blob_path(const fs::path& cache_root, const std::string& id) {
    if (!blob_id_ok(id)) return {};
    return cache_root / "blobs" / id.substr(0, 2) / id.substr(2);
}

std::string blob_id_of(const fs::path& file) {
    return text::sha1_hex(read_all(file));
}

bool blob_present(const fs::path& cache_root, const std::string& id) {
    const auto p = blob_path(cache_root, id);
    if (p.empty()) return false;
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

std::string blob_store(const fs::path& cache_root, const std::string& id,
                       const std::string& bytes) {
    if (!blob_id_ok(id)) return "内容指纹不合法：要 40 个小写十六进制字符";

    // **先核对再落地。** 截断的 png 照样"存下来了"，之后报的是
    // "权重读不对"或者一张半截图，指向完全错误的方向。
    const std::string real = text::sha1_hex(bytes);
    if (real != id) {
        return "内容对不上指纹：说好的是 " + id + "，收到的是 " + real +
               "（" + std::to_string(bytes.size()) + " 字节）。多半是传了一半";
    }

    const auto dest = blob_path(cache_root, id);
    std::error_code ec;
    if (fs::is_regular_file(dest, ec)) return {};   // 本来就有

    fs::create_directories(dest.parent_path(), ec);
    if (ec) return "建不出目录：" + paths::to_utf8(dest.parent_path());

    // **先写临时文件再改名。** 直接往目标写的话，写到一半进程没了就留下
    // 一个"指纹对得上文件名、内容却是半截"的 blob——而之后每一次
    // `blob_present` 都会说它在，再也不会重传。改名是原子的，没有这个洞。
    const auto tmp = dest.parent_path() /
                     (dest.filename().string() + ".part");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return "写不了：" + paths::to_utf8(tmp);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) return "写坏了：" + paths::to_utf8(tmp);
    }
    fs::rename(tmp, dest, ec);
    if (ec) {
        // Windows 上目标已存在时 rename 会失败。那说明别人刚好也存了
        // 同一份——内容一样，无所谓谁的，清掉临时文件当成功。
        std::error_code ec2;
        if (fs::is_regular_file(dest, ec2)) {
            fs::remove(tmp, ec2);
            return {};
        }
        return "改名失败：" + paths::to_utf8(dest);
    }
    return {};
}

std::string blob_adopt(const fs::path& cache_root, const fs::path& file) {
    const std::string bytes = read_all(file);
    const std::string id = text::sha1_hex(bytes);
    const auto why = blob_store(cache_root, id, bytes);
    if (!why.empty()) throw std::runtime_error(why);
    return id;
}

}  // namespace changji::infer
