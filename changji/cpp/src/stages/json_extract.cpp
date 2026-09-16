#include "stages/json_extract.hpp"

#include "stages/json_partial.hpp"

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

    // **最后一道：把半截的补齐再试一次。**
    //
    // 上面三条全是"整份必须是完好的"：直接解、围栏里那段、第一个括号平衡
    // 的片段。模型写到一半被掐断（长度上限、连接抖一下）就一条都不成立，
    // 而那时候前面写好的东西其实都在。2026-09-16 实测：写一章十分钟，
    // 日志里 JSON 开头一切正常（scenes 数组、第一场的 where/pov/who 全齐），
    // 就因为结尾没闭合，整份作废、一个字不剩。
    //
    // `close_partial_json` 本来就是干这个的（写大纲那条边写边看的路在用）。
    // 它**从头扫**，所以只管"开头就是 JSON、结尾没写完"这一种；前面还带着
    // 「好的，这是结果：」之类前缀的那些，仍然归上面 find_balanced 管。
    //
    // 放在最后、只在前面全败之后跑：这条路原来百分之百是抛异常，所以它
    // 只可能把"失败"变成"成功"，不会改变任何一个本来就解得出来的结果。
    // 补出来的那份可能缺东西——**那交给下游的 schema 校验去判**，
    // 这儿只负责"能解出多少算多少"。
    if (const std::string closed = close_partial_json(t); !closed.empty()) {
        json salvaged = json::parse(closed, nullptr, /*allow_exceptions=*/false);
        // **补出来得有东西。** `{不平衡的括号` 这种补完是个空对象——它能解，
        // 但里面什么都没有，当成功往下游送比在这儿抛更糟：下一步会拿着一份
        // 空数据再报一个更难懂的错。语料里钉着这一条要抛。
        if (!salvaged.is_discarded() && !salvaged.empty() &&
            (salvaged.is_object() || salvaged.is_array())) {
            return salvaged;
        }
    }

    // 截 400 字符，和 Python 一致。全贴出来的话，一段几万字的模型输出
    // 会把日志和界面的错误框都撑爆。
    // **按字符截，不是按字节。** 这条消息会进任务快照再序列化成 JSON，
    // 字节截断落在半个汉字上时整个 /api/script/series 都回 500，进度就
    // 看不见了——2026-09-11 实跑撞上的就是这个。
    throw std::runtime_error("大模型输出里找不到合法 JSON：\n" + text::truncate_utf8(raw, 400));
}

}  // namespace changji::stages
