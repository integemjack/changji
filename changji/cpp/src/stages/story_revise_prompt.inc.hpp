// 改原稿某一段的提示词。**这个文件就是出处，直接改它。**
// 新写的，没有 Python 对应物，不受逐字节约束。
//
// 拼接顺序：Head + 画风 + Rules + 全局记忆 + 上下文
//           [+ HistoryHead + 来回] + TaskHead + 这次要做什么 + Tail

#pragma once

namespace changji::stages::prompt {

inline constexpr const char* kReviseHead =
    R"CJ(你在帮一位作者改他自己写的)CJ";

inline constexpr const char* kReviseHintRealistic =
    R"CJ(真人写实短剧)CJ";

inline constexpr const char* kReviseHintAnime =
    R"CJ(动漫短剧)CJ";

/// **第一条是这一步的全部意义。**
///
/// 用户敢把写了一半的稿子交给 AI，靠的是"它只动我圈出来的那几行"。一旦
/// 它顺手改了别处，哪怕改得更好，这个功能也就没人敢按第二次了——因为他
/// 没法知道还有哪儿被动过。
inline constexpr const char* kReviseRules =
    R"CJ(的原作正文。

**只改选中的那一段，别的一个字都不要动。** 作者圈出来的就是他想改的
地方；把前后文也一起重写的话，他上周调好的句子会被悄悄换掉，而他要到
很久以后才发现。

其余要求：
1. **回来的只是替换选中那一段的新正文**，不要把整章抄回来，也不要带
   章节标题、编号、「修改后：」这类字样。
2. 接得上前后文：开头接得住前面那段的最后一句，结尾接得上后面那段的
   第一句。
3. 写**小说体**：第三人称，有动作有对白有环境。不要写「场景一」
   「（画外音）」这类分镜标记——后面另有一步把正文变成拍子。
4. 人物名和下面给的**一字不差**，不要冒出没在人物表里的人。
5. **不要描写长相、发型、服装。** 人物长什么样由后面的美术那一步统一定，
   这里写了两边必然不一致。
6. 作者没要求改长短的话，篇幅和原来那段差不多就行。

)CJ";

inline constexpr const char* kReviseHistoryHead =
    R"CJ(
【之前聊过什么】
)CJ";

inline constexpr const char* kReviseTaskHead =
    R"CJ(
【这次要你做什么】
)CJ";

inline constexpr const char* kReviseTail =
    R"CJ(

把改完的那一段放进 text；另外用一句话说你改了什么，放进 note。
note 是说给作者听的，不进正文。
)CJ";

}  // namespace changji::stages::prompt
