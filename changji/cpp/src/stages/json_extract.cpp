#include "stages/json_extract.hpp"

#include <stdexcept>
#include <string>

#include "util/text.hpp"

using json = nlohmann::json;

namespace changji::stages {

namespace {

/// 找 ``` 代码块里的内容，对应 Python 的
/// re.search(r"```(?:json)?\s*(.+?)```", text, re.S)。
///
/// 手写而不用 std::regex：std::regex 在长输入上会递归到爆栈，
/// 而这里的输入正是"大模型吐的一大段文本"，动辄几万字。
/// 这个模式本身简单到不值得为它冒那个险。
bool find_fenced(const std::string& text, std::string& out) {
    const std::string kFence = "```";
    const std::size_t open = text.find(kFence);
    if (open == std::string::npos) return false;

    std::size_t i = open + kFence.size();
    // (?:json)? —— 可有可无的语言标注
    if (text.compare(i, 4, "json") == 0) i += 4;
    // \s* 是贪婪的，但后面的 (.+?) 至少要一个字符
    while (i < text.size() &&
           (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' ||
            text[i] == '\r' || text[i] == '\v' || text[i] == '\f')) {
        ++i;
    }
    if (i >= text.size()) return false;

    // (.+?) 非贪婪，所以取到**最近**的那个闭合围栏
    const std::size_t close = text.find(kFence, i + 1);
    if (close == std::string::npos) return false;

    out = text::strip_ws(text.substr(i, close - i));
    return true;
}

/// 找第一个括号平衡的片段。
///
/// 刻意不管字符串里的括号——Python 那边也不管。
/// 内容里有 "}" 的话两边都会失败，行为一致比"我这边更聪明"重要：
/// 对拍时一个能过一个不能过，查起来比两个都失败麻烦得多。
bool find_balanced(const std::string& text, char opener, char closer,
                   json& out) {
    const std::size_t start = text.find(opener);
    if (start == std::string::npos) return false;

    int depth = 0;
    for (std::size_t idx = start; idx < text.size(); ++idx) {
        if (text[idx] == opener) {
            ++depth;
        } else if (text[idx] == closer) {
            --depth;
            if (depth == 0) {
                json parsed = json::parse(text.substr(start, idx - start + 1),
                                          nullptr, /*allow_exceptions=*/false);
                if (parsed.is_discarded()) return false;  // 对应 Python 的 break
                out = std::move(parsed);
                return true;
            }
        }
    }
    return false;
}

}  // namespace

json extract_json(const std::string& raw) {
    std::string t = text::strip_ws(raw);
    std::string fenced;
    if (find_fenced(t, fenced)) t = fenced;

    json direct = json::parse(t, nullptr, /*allow_exceptions=*/false);
    if (!direct.is_discarded()) return direct;

    json out;
    if (find_balanced(t, '{', '}', out)) return out;
    if (find_balanced(t, '[', ']', out)) return out;

    // 截 400 字符，和 Python 一致。全贴出来的话，一段几万字的模型输出
    // 会把日志和界面的错误框都撑爆。
    // **按字符截，不是按字节。** 这条消息会进任务快照再序列化成 JSON，
    // 字节截断落在半个汉字上时整个 /api/script/series 都回 500，进度就
    // 看不见了——2026-09-11 实跑撞上的就是这个。
    throw std::runtime_error("大模型输出里找不到合法 JSON：\n" + text::truncate_utf8(raw, 400));
}

}  // namespace changji::stages
