#include "util/text.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

namespace changji::text {

namespace {

/// 要从各段尾部剥掉的标点，对应 Python 的 rstrip("。.；;，,、 ")。
const std::array<const char*, 8> kTrimChars = {
    "。", ".", "；", ";", "，", ",", "、", " "
};

bool is_ascii_ws(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\v' || c == '\f';
}

std::uint32_t rotl(std::uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

}  // namespace

std::string rstrip_punct(std::string s) {
    bool changed = true;
    while (changed && !s.empty()) {
        changed = false;
        for (const char* p : kTrimChars) {
            const std::size_t n = std::char_traits<char>::length(p);
            if (s.size() >= n && s.compare(s.size() - n, n, p) == 0) {
                s.erase(s.size() - n);
                changed = true;
                break;
            }
        }
    }
    return s;
}

std::string strip_ws(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && is_ascii_ws(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && is_ascii_ws(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string collapse_ws(const std::string& s) {
    // 只压 ASCII 空白。
    //
    // Python 的 re.sub(r"\s+", " ", s) 在 str 上是 Unicode 感知的，
    // 还会匹配全角空格 U+3000、不换行空格 U+00A0 之类。这里不匹配。
    //
    // 差异是真实存在的，但影响面很小：这些字符要真出现在模型输出里，
    // Python 会把它压成半角空格，这里会原样留着。两边都不产生乱码，
    // 只是提示词里多一个全角空格。为这个引一整套 Unicode 表不划算——
    // 真遇到再说，届时应该做的是在 Unicode 属性表里查而不是硬编码一串字符。
    std::string out;
    out.reserve(s.size());
    bool in_ws = false;
    for (const char c : s) {
        if (is_ascii_ws(static_cast<unsigned char>(c))) {
            if (!in_ws) out.push_back(' ');
            in_ws = true;
        } else {
            out.push_back(c);
            in_ws = false;
        }
    }
    return out;
}

std::string clean_field(const std::string& s) {
    return rstrip_punct(strip_ws(collapse_ws(s)));
}

std::size_t utf8_len(const std::string& s) {
    std::size_t n = 0;
    for (const char ch : s) {
        // 续接字节是 10xxxxxx，只数不是续接的
        if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80) ++n;
    }
    return n;
}

std::size_t utf8_char_len(unsigned char lead) {
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

std::string truncate_utf8(const std::string& s, std::size_t n) {
    std::size_t chars = 0, i = 0;
    while (i < s.size() && chars < n) {
        const std::size_t len =
            utf8_char_len(static_cast<unsigned char>(s[i]));
        if (i + len > s.size()) break;  // 尾部本来就是残的，就到这儿
        i += len;
        ++chars;
    }
    return s.substr(0, i);
}

std::vector<std::string> utf8_chars(const std::string& s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        const std::size_t n = utf8_char_len(static_cast<unsigned char>(s[i]));
        // 尾部残缺时按剩下的取。丢掉的话字数会少，估出来的时长偏短，
        // 而偏短的表现是配音装不进镜头。
        const std::size_t take = std::min(n, s.size() - i);
        out.push_back(s.substr(i, take));
        i += take;
    }
    return out;
}

char32_t utf8_codepoint(const std::string& ch) {
    if (ch.empty()) return 0;
    const auto b0 = static_cast<unsigned char>(ch[0]);
    const auto cont = [&ch](std::size_t i) {
        return static_cast<char32_t>(static_cast<unsigned char>(ch[i]) & 0x3F);
    };
    if (b0 < 0x80) return b0;
    if ((b0 & 0xE0) == 0xC0 && ch.size() >= 2) {
        return (static_cast<char32_t>(b0 & 0x1F) << 6) | cont(1);
    }
    if ((b0 & 0xF0) == 0xE0 && ch.size() >= 3) {
        return (static_cast<char32_t>(b0 & 0x0F) << 12) | (cont(1) << 6) | cont(2);
    }
    if ((b0 & 0xF8) == 0xF0 && ch.size() >= 4) {
        return (static_cast<char32_t>(b0 & 0x07) << 18) | (cont(1) << 12) |
               (cont(2) << 6) | cont(3);
    }
    return 0;
}

std::string sha1_raw(const std::string& data) {
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu,
                          0x10325476u, 0xC3D2E1F0u};

    // 补位：先加一个 0x80，再补零到 56 字节，最后 8 字节是**比特**长度大端。
    // 长度写字节数是最经典的实现错误，摘要会全错但看着依然像个正常的 SHA-1。
    std::string msg = data;
    const std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back('\0');
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<char>((bit_len >> (i * 8)) & 0xFF));
    }

    for (std::size_t off = 0; off < msg.size(); off += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const auto* p =
                reinterpret_cast<const unsigned char*>(msg.data() + off + i * 4);
            w[i] = (static_cast<std::uint32_t>(p[0]) << 24) |
                   (static_cast<std::uint32_t>(p[1]) << 16) |
                   (static_cast<std::uint32_t>(p[2]) << 8) |
                   static_cast<std::uint32_t>(p[3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);            k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                     k = 0xCA62C1D6u; }
            const std::uint32_t tmp = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = tmp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    // 原始 20 字节，大端。十六进制那版在下面套一层。
    std::string out;
    out.reserve(20);
    for (const std::uint32_t v : h) {
        for (int i = 3; i >= 0; --i) {
            out.push_back(static_cast<char>((v >> (i * 8)) & 0xFF));
        }
    }
    return out;
}

std::string sha1_hex(const std::string& data) {
    const std::string raw = sha1_raw(data);
    std::string out;
    out.reserve(40);
    for (const char c : raw) {
        char buf[3];
        std::snprintf(buf, sizeof(buf), "%02x",
                      static_cast<unsigned>(static_cast<unsigned char>(c)));
        out.append(buf, 2);
    }
    return out;
}

std::string base64_encode(const std::string& data) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const auto a = static_cast<unsigned char>(data[i]);
        const auto b = static_cast<unsigned char>(data[i + 1]);
        const auto c = static_cast<unsigned char>(data[i + 2]);
        out.push_back(kAlphabet[a >> 2]);
        out.push_back(kAlphabet[((a & 0x03) << 4) | (b >> 4)]);
        out.push_back(kAlphabet[((b & 0x0F) << 2) | (c >> 6)]);
        out.push_back(kAlphabet[c & 0x3F]);
    }
    // 尾巴要补 '='。少补的话 WebSocket 握手的 Sec-WebSocket-Key 长度不对，
    // 服务端直接拒绝连接——而错误只是"连不上"，看不出是编码问题。
    if (i < data.size()) {
        const auto a = static_cast<unsigned char>(data[i]);
        out.push_back(kAlphabet[a >> 2]);
        if (i + 1 < data.size()) {
            const auto b = static_cast<unsigned char>(data[i + 1]);
            out.push_back(kAlphabet[((a & 0x03) << 4) | (b >> 4)]);
            out.push_back(kAlphabet[(b & 0x0F) << 2]);
        } else {
            out.push_back(kAlphabet[(a & 0x03) << 4]);
            out.push_back('=');
        }
        out.push_back('=');
    }
    return out;
}

std::string project_slug(const std::string& name) {
    // 对应 re.sub(r"[^a-z0-9_-]+", "-", name.lower()).strip("-")
    std::string out;
    out.reserve(name.size());
    bool prev_sep = false;
    for (const char ch : name) {
        const unsigned char c = static_cast<unsigned char>(ch);
        char lowered = static_cast<char>(c);
        if (c >= 'A' && c <= 'Z') lowered = static_cast<char>(c - 'A' + 'a');
        const bool keep = (lowered >= 'a' && lowered <= 'z') ||
                          (lowered >= '0' && lowered <= '9') ||
                          lowered == '_' || lowered == '-';
        if (keep) {
            out.push_back(lowered);
            prev_sep = false;
        } else if (!prev_sep) {
            out.push_back('-');
            prev_sep = true;
        }
    }
    std::size_t b = 0, e = out.size();
    while (b < e && out[b] == '-') ++b;
    while (e > b && out[e - 1] == '-') --e;
    const std::string trimmed = out.substr(b, e - b);
    if (!trimmed.empty()) return trimmed;
    // 纯中文目录名走这条
    return "p-" + sha1_hex(name).substr(0, 8);
}

std::string slug(const std::string& text) {
    // 按字节处理就够，不用解 UTF-8。
    //
    // 非 ASCII 的字节一个都不在 [a-z0-9_] 里，而正则是 [^a-z0-9_]+ ——
    // 带加号，连续的都塌成一个下划线。所以一个 3 字节的汉字和一个 ASCII
    // 标点，结果都是一个下划线，和 Python 按字符处理的结果一致。
    //
    // 有一个理论上的差异：Python 的 str.lower() 是 Unicode 感知的，
    // 极少数字符小写后会产生 ASCII 字母（比如土耳其语 İ 小写成 i 加组合
    // 附加符）。那种情况这里会多出一个下划线。中文短剧的角色名不会碰到，
    // 真碰到了正确的做法是接一套 Unicode 大小写映射，而不是打补丁。
    std::string out;
    out.reserve(text.size());
    bool prev_sep = false;
    for (const char ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        char lowered = static_cast<char>(c);
        if (c >= 'A' && c <= 'Z') lowered = static_cast<char>(c - 'A' + 'a');
        const bool keep = (lowered >= 'a' && lowered <= 'z') ||
                          (lowered >= '0' && lowered <= '9') || lowered == '_';
        if (keep) {
            out.push_back(lowered);
            prev_sep = false;
        } else if (!prev_sep) {
            out.push_back('_');
            prev_sep = true;
        }
    }

    // 对应 Python 的 .strip("_")
    std::size_t b = 0, e = out.size();
    while (b < e && out[b] == '_') ++b;
    while (e > b && out[e - 1] == '_') --e;
    const std::string trimmed = out.substr(b, e - b);

    if (!trimmed.empty()) return trimmed;
    // 全是非法字符（纯中文名走这条），用内容摘要兜底，保证 id 稳定且唯一
    return "x" + sha1_hex(text).substr(0, 6);
}

}  // namespace changji::text
