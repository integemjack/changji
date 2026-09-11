// 逐章展开正文的提示词。**这个文件就是出处，直接改它。**
// 新写的，不受逐字节约束。
//
// 这一步补的是 AI 那条路上最后一个洞：大纲写出来的故事**只有梗概没有正文**，
// 写剧本时展开的是「这一章发生什么，三五句」，而不是真正的内容。按字符切分
// 那套机器（episode_text）在这条路上一直返回空，退回用梗概。
//
// 拼接顺序：Seg0 + 画风 + Seg1 + 目标字数 + Seg2 + Rules
//           + ContextHead + 上下文 + Tail

#pragma once

namespace changji::stages::prompt {

inline constexpr const char* kChapterSeg0 =
    R"CJ(你在写一部竖屏)CJ";

inline constexpr const char* kChapterHintRealistic =
    R"CJ(真人写实短剧)CJ";

inline constexpr const char* kChapterHintAnime =
    R"CJ(动漫短剧)CJ";

inline constexpr const char* kChapterSeg1 =
    R"CJ(的原作，现在写其中**一章**的正文，大约 )CJ";

inline constexpr const char* kChapterSeg2 =
    R"CJ( 字。

**主要产出是正文**——几千字连贯的叙述，不是标题也不是梗概。另外顺带标出 )CJ";

inline constexpr const char* kChapterSeg3 =
    R"CJ( 个可以收一集的地方。

硬性要求：
)CJ";

inline constexpr const char* kChapterRules =
    R"CJ(1. **正文要写满上面那个字数**。写出一两句话、或者把章名填进去当正文，
   都算没写——后面每一集都是照着这段正文展开的，正文不够就无米下锅。
2. **只写这一章。** 后面几章的事一个字都不要提前写——那些是后面的内容，
   提前写了到那一章就没东西可拍了。
3. **结尾必须停在下面给的那个钩子上。** 这一章的最后几段就是奔着它去的。
4. 写**小说体**，不是剧本格式：第三人称，有动作有对白有环境。
   不要写「场景一」「（画外音）」这类分镜/剧本的标记——后面另有一步把它
   变成拍子，那一步认的是连贯的正文。
5. 人物名和下面给的**一字不差**，不要冒出没在人物表里的人。
6. **不要描写长相、发型、服装。** 人物长什么样由后面的美术那一步统一定，
   这里写了两边必然不一致。
7. 换地方、换时间的时候交代清楚：在哪、什么时候、什么光。后面每一镜都要
   照着这些字画出画面来。
8. 提到的东西要具体到看得见：不说「一件信物」，说「一枚缺了角的铜钱」。
9. 对白要能直接说出口，别写成书面语。
10. hooks 里每一条是**一个可以收一集的地方**：text 写那儿悬着的是什么
   （悬念、反转，或者一个明确的情绪落点），after 照抄**那个位置前面
   那句的原文，十到二十个字**。
   · 按正文里的先后排，**最后一条是章尾**。
   · after 必须是正文里**一字不差**的原句，程序要拿它去查位置。抄错了或者
     自己编，那一条就落不下去，那一集只能收在一个说不出为什么的地方——
     而每一集的结尾是完播率的命门。
   · 几个钩子要**大致均匀地散在这一章里**，别全挤在结尾：它们各自是一集的
     收尾，挤在一起等于前面几集没有收尾。
   · 写正文的时候就往这几个点上使劲——先想好在哪儿断，再写到那儿。

)CJ";

inline constexpr const char* kChapterContextHead =
    R"CJ(下面是这部剧的底子和这一章要写的内容。

)CJ";

inline constexpr const char* kChapterTail =
    R"CJ(

只输出 JSON，不要任何解释文字。)CJ";

// 塞进提示词前的截断长度，按**字符**不是字节。
inline constexpr std::size_t kChapterRecapMaxChars = 2000;
inline constexpr std::size_t kChapterPrevTailMaxChars = 400;

// 一章最长写多少字。模型偶尔会失控往下写个没完，而那一章的正文会整份存进
// story.json，还会被后面每一集的提示词读。
inline constexpr std::size_t kChapterMaxChars = 20000;

}  // namespace changji::stages::prompt
