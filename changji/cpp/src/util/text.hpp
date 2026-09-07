#pragma once

// 文本处理。都是从 Python 侧一比一搬过来的，每个都对应一处具体的
// 字符串操作——这些函数的输出会进提示词，而提示词要求逐字节一致，
// 所以这里没有"差不多就行"的余地。

#include <cstddef>
#include <string>

namespace changji::text {

/// 剥掉尾部的空白和中英文标点，对应 Python 的 rstrip("。.；;，,、 ")。
///
/// 必须按 UTF-8 字符剥，不能按字节。中文标点每个 3 字节，
/// 按字节剥会把前一个汉字劈成半个，产出乱码——而这个串会出现在
/// 每一个镜头的提示词里，一旦坏掉是全剧性的。
std::string rstrip_punct(std::string s);

/// 对应 Python 的 str.strip()：只剥 ASCII 空白，两端都剥。
std::string strip_ws(const std::string& s);

/// 对应 Python 的 re.sub(r"\s+", " ", s)：连续空白压成一个空格。
///
/// 只认 ASCII 空白。Python 的 \s 在 str 上默认也匹配 Unicode 空白
/// （比如全角空格 U+3000），这里不匹配——见 .cpp 里的说明。
std::string collapse_ws(const std::string& s);

/// 清洗模型给的字段：压空白、两端去空白、剥尾部标点。
///
/// 对应 bible.py 的 _clean()。尾部标点必须去掉，因为这些字段拼提示词时
/// 用逗号连接，模型带来的句号会让结果变成"冷静克制。，身姿笔挺。，"
/// 这样标点重复的串，而且这个串会出现在每一个镜头里。
std::string clean_field(const std::string& s);

/// UTF-8 字符数（不是字节数）。
///
/// 靠"续接字节高两位是 10"来数，不解码码点。中文一个字三字节，
/// 用 s.size() 当字数会让所有长度判断偏大三倍。
std::size_t utf8_len(const std::string& s);

/// 一个 UTF-8 字符占几字节，从起始字节判断。非法起始字节按 1 算。
std::size_t utf8_char_len(unsigned char lead);

/// 按 UTF-8 字符截断到最多 n 个字符。
///
/// 不能直接 substr：中文一个字 3 字节，按字节截会把最后一个字劈成半个，
/// 产出非法 UTF-8。这个串会进错误消息，而错误消息要序列化成 JSON——
/// nlohmann 遇到非法 UTF-8 会抛异常，于是"报错"这件事本身又炸一次。
std::string truncate_utf8(const std::string& s, std::size_t n);

/// SHA-1，返回小写十六进制。
///
/// 只为 slug() 的退路存在。不引第三方库是因为整个项目就这一处用到摘要，
/// 为它拉一个 OpenSSL 或者 cryptopp 不划算——交叉编译到树莓派时
/// 那些库的配置是另一套要维护的东西。
std::string sha1_hex(const std::string& data);

/// SHA-1，返回**原始 20 字节**。
///
/// WebSocket 握手要的是这个：Sec-WebSocket-Accept 是
/// base64(sha1(key + 固定 GUID))，中间不经过十六进制。
/// 拿 sha1_hex 的输出去 base64 会得到一个 56 字符的串，
/// 服务端算出来的是 28 字符——握手失败，而报错只是"连不上"。
std::string sha1_raw(const std::string& data);

/// 标准 base64（带 = 补齐，不是 URL 变体）。
std::string base64_encode(const std::string& data);

/// 目录名转成合法的项目 id，对应 web/server.py 的 _slugify()。
///
/// **和下面那个 slug() 规则不一样**，别合并：分隔符是连字符不是下划线，
/// 退路的前缀是 "p-" 且取 8 位摘要不是 6 位。两处产生的 id 进的是不同的
/// 字段（项目 id vs 角色 id），合并会让其中一边的 id 悄悄变形，
/// 而 id 变形意味着老项目打不开。
std::string project_slug(const std::string& name);

/// 转成合法的 id 片段，对应 bible.py 的 _slug()。
///
/// 规则：小写，非 [a-z0-9_] 的连续片段换成一个下划线，两端去下划线。
/// 结果为空时用 "x" 加内容 SHA-1 的前 6 位——中文名会走到这条退路。
std::string slug(const std::string& text);

}  // namespace changji::text
