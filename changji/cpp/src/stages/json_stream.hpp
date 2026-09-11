#pragma once

// 从正在生成的 JSON 里，边生边把某一个字段的字符串值抠出来。
//
// **为什么要这么个东西**：用户要"AI 生成在编辑器里流式插入"。改一小段话
// 可以让模型直接吐大白话（见 story_revise 的 plain 那条），但**写一整章
// 不行**——那一步除了正文还要模型标出"这一章里哪几个地方可以收一集"
// （hooks），而那些钩子是一集停在真悬念上的全部依据：实跑里它把
// "停在真悬念上"的比例从 25% 抬到 56%。为了能流式就把 hooks 砍掉，等于拿
// 分集质量换一个动画。
//
// 所以正文那一步照旧约束成 JSON，只是在 token 流上**顺手把 text 那个字段
// 解出来**推给编辑器。JSON 外壳（`{"text":"`、结尾那串 hooks）不推。
//
// 纯状态机，不碰网络也不碰模型。**单独成文件是为了能测**：它要处理转义、
// \uXXXX、以及"一个转义序列被切在两个 token 中间"——最后这条是流式独有的，
// 攒齐了再解析的代码永远碰不到，而它错了的表现是正文里凭空多出几个
// 反斜杠。

#include <cstdint>
#include <string>

namespace changji::stages {

/// 从 JSON token 流里抠一个顶层字符串字段。
///
/// 用法：每来一段 token 就 feed 一次，返回**这一次新解出来的字符**（可能
/// 是空串）。字段的字符串一结束，done() 变真，之后 feed 一律返回空。
///
/// 只认**顶层对象的第一层**键。嵌套对象里的同名键不认——这一层不需要，
/// 而认了就得维护一个真正的 JSON 栈。
class JsonFieldStreamer {
public:
    explicit JsonFieldStreamer(std::string field);

    /// 喂一段原始文本，拿回新解出来的字符。
    std::string feed(const std::string& piece);

    /// 这个字段的值收完了没有。
    bool done() const { return state_ == State::Done; }

    /// 到目前为止解出来的全部。收尾时可以拿它和权威那份对一下。
    const std::string& text() const { return out_; }

private:
    enum class State {
        SeekKey,    ///< 在找下一个 `"`（可能是键的开头）
        InKey,      ///< 正在读一个键名
        AfterKey,   ///< 键名读完了，等冒号
        SeekValue,  ///< 等值那个 `"`
        InValue,    ///< 正在读值，这一段才往外吐
        Escape,     ///< 值里遇到了 `\`
        Unicode,    ///< 正在收 \uXXXX 的四位
        Done,
    };

    std::string field_;
    State state_ = State::SeekKey;
    std::string key_;      ///< 正在读的键名
    std::string out_;      ///< 已经解出来的全部
    std::string hex_;      ///< \uXXXX 收到一半的那几位
    std::uint16_t high_ = 0;  ///< 代理对的高位，等低位来配
    bool key_escape_ = false;  ///< 键名里的转义。键名一般没有，但不能崩
};

}  // namespace changji::stages
