// 从故事写一集的提示词。**这个文件就是出处，直接改它。**
//
// 和隔壁 script_prompt.inc.hpp 里那份「正片」不是一回事。那一份是
// 逐集续写：给一句梗概加前三集原文，让模型自己想这一集该发生什么。
// 于是没有全局结构、写到第五集开始失忆（前三集之外的事不在上下文里）、
// 故事也永远没有终点。
//
// 这一份反过来：**这一集要发生什么已经定好了**，模型只负责把那一段故事
// 变成能拍的拍子。上下文里带的是压缩的全局记忆（大纲、人物、关系、前情
// 提要），不是前几集的原文——所以写第二十集时第二章埋的伏笔照样在。
//
// 新写的，没有 Python 对应物，**不受逐字节约束**。
//
// 拼接顺序：Seg0 + 时长 + Seg1 + 画风 + Seg2 + 字数 + Rules
//           + [CharsPre + 角色名 + CharsPost]
//           + ContextHead + 渲染出来的上下文 + Tail

#pragma once

namespace changji::stages::prompt {

inline constexpr const char* kStoryScriptSeg0 =
    R"CJ(你在把一部剧里的一集写成剧本，这一集总时长约 )CJ";

inline constexpr const char* kStoryScriptSeg1 =
    R"CJ( 秒。)CJ";

inline constexpr const char* kStoryScriptSeg2 =
    R"CJ(。

**这一集要发生什么已经定好了**，下面「这一集」那一段就是。你的活是把它
变成能拍的拍子，不是重新构思剧情。

硬性要求：
1. 所有对白加起来控制在 )CJ";

inline constexpr const char* kStoryScriptRules =
    R"CJ( 个字左右，超出很多就是拍不完。
2. **只拍「这一集」那一段。** 前情提要是让你知道之前发生过什么，
   不要把前情再演一遍——那是上一集已经拍过的画面。
3. **结尾必须停在给定的那个钩子上。** 下面写了这一集要停在哪，
   最后一两拍就是奔着它去的。提前收或者越过去，下一集就接不上。
4. 动作和对白交替推进，不要连着好几句对白，画面会没有呼吸。
5. 出场角色不超过三个。人一多，短剧里根本立不住。
6. **必须有台词，而且每一句都要落到具体的人身上。**
   正文里有人说的话，照着写成台词；正文只是叙述、没明说谁说了什么的地方，
   该有对话就写出来——这是把小说改成剧本，不是把小说念一遍。
   **speaker 一律填人物表里的名字，不能留空**：留空的那句会被当成旁白并入
   动作，整集一句台词都没有的话，出来的是默片。
7. 对白只写说出口的话，不要带引号，不要在 text 里重复人名。
8. 人物名必须和上面给的一模一样，不要一会儿全名一会儿简称，
   也不要冒出没在人物表里的人。
9. 开头三秒就要有事发生，短剧没有铺垫的余地。

动作那几拍要**拍得出来**。下一步要照着它画分镜，所以：
10. 换地方、换时间就在那一拍的开头交代清楚：在哪、什么时候、什么光。
   例如「深夜，天台，只有应急灯」。同一个地方连着几拍就不用重复。
11. 写角色**身体在做什么**，别写他心里怎么想。
    不要「她很生气」，要「她把茶杯重重放下，茶水溅到手背上」。
    情绪从动作和台词里透出来，写心理活动画不出来。
12. 提到的东西要具体到看得见：不说「一件信物」，说「一枚缺了角的铜钱」。
13. 每一拍只写一件事。一拍里塞三个动作，分镜只能挑一个画，剩下的就丢了。

)CJ";

inline constexpr const char* kStoryScriptCharsPre =
    R"CJ(必须沿用这些已有角色，名字一字不改：)CJ";

inline constexpr const char* kStoryScriptCharsPost =
    R"CJ(。

)CJ";

inline constexpr const char* kStoryScriptContextHead =
    R"CJ(下面是这部剧的底子和这一集要拍的内容。

)CJ";

inline constexpr const char* kStoryScriptTail =
    R"CJ(

只输出 JSON，不要任何解释文字。)CJ";

// 塞进提示词前的截断长度，按**字符**不是字节。
//
// 前情提要给得比正文紧：它是压缩过的（每章一句），给多了会把「这一集」
// 那一段挤到上下文末尾，而模型对末尾最容易糊弄过去。
inline constexpr std::size_t kStoryRecapMaxChars = 2000;
inline constexpr std::size_t kStoryEpisodeMaxChars = 6000;
inline constexpr std::size_t kStoryPrevTailMaxChars = 300;

}  // namespace changji::stages::prompt
