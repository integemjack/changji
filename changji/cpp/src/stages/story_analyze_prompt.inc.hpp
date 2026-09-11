// 读一遍现成的正文，把人物表、关系、地点和每章的梗概钩子提出来。
// **这个文件就是出处，直接改它。** 新写的，不受逐字节约束。
//
// 和 story_outline 那份的分工：
//
//   outline   无中生有：从一句梗概编出整个故事
//   analyze   已经有正文了，只是**读它**，把结构提出来
//
// 两件事不能合成一个提示词。让「写故事」那份去读现成的正文，它会忍不住
// 改写——写出来的东西和用户粘进来的对不上，而后面每一集都是照着正文展开的。
//
// 拼接顺序：Seg0 + 画风 + Seg1 + Rules + ChaptersHead + 章节节选 + Tail

#pragma once

namespace changji::stages::prompt {

inline constexpr const char* kAnalyzeSeg0 =
    R"CJ(下面是一部)CJ";

inline constexpr const char* kAnalyzeHintRealistic =
    R"CJ(真人写实短剧)CJ";

inline constexpr const char* kAnalyzeHintAnime =
    R"CJ(动漫短剧)CJ";

inline constexpr const char* kAnalyzeSeg1 =
    R"CJ(要用的现成文本，已经分好章了。
读一遍，把里面的人、关系、地方和每一章的梗概提出来。

硬性要求：
)CJ";

inline constexpr const char* kAnalyzeRules =
    R"CJ(1. **只提文本里真实出现的东西，不要自己加人加地方。** 加出来的东西在
   正文里没有对应，后面照着正文写剧本时它就凭空消失了。
2. **不要改写正文，也不要续写。** 你的活是读和归纳。后面每一集都是照着
   这份正文展开的，你在这里编的东西不会出现在成片里。
3. 人物名**照抄正文里的写法**，一字不改。正文里一会儿全名一会儿简称的，
   统一用出现得最多的那个。后面每一镜都按名字找角色。
4. **不要写长相、发型、服装。** 人物长什么样由后面的美术那一步统一定，
   这里写了两边必然不一致。只写他是谁、他要什么、他怎么变。
5. 每一组有戏的关系都要写进 relations，并说清那段关系里绷着的是什么。
6. 每一章的 summary 写这一章发生了什么，三五句，是归纳不是摘抄。
7. **每一章要标好几个可以收一集的地方，不是只标章尾。** 一章会按每集时长
   切成好几集，只给章尾那一个的话，前面几集只能收在一个说不出为什么的
   地方——而每一集的结尾是完播率的命门。
   hooks 里每一条：text 写那儿悬着的是什么（悬念、反转，或明确的情绪
   落点），after 照抄**那个位置前面那句的原文，十到二十个字**。
8. after 必须是节选里**一字不差**的原句，程序要拿它去查位置。抄错了或者
   自己编，那一条就落不下去。**节选里带「……（中间略）……」的地方不要
   在那儿标钩子**，那段正文你没看到，抄不出原句。
9. hooks 按正文里的先后排，最后一条是章尾。几个钩子大致均匀散开，
   别全挤在结尾。
10. chapter_id 照抄下面给的，不要改、不要漏、不要自己编号。
11. 节选里带「……」的地方是中间省略掉的正文，不要把省略号当成情节。

)CJ";

inline constexpr const char* kAnalyzeChaptersHead =
    R"CJ(章节如下：

)CJ";

inline constexpr const char* kAnalyzeTail =
    R"CJ(

只输出 JSON，不要任何解释文字。)CJ";

// 章节节选总共塞多少字。**按字符不是字节。**
//
// 整本小说塞不进上下文，所以每章只取头尾：开头交代这一章从哪儿接上，
// 结尾决定钩子在哪，中间的过程靠这两头能推个八九不离十。
// 章多的时候每章分到的更少，但至少每章都在——漏掉一整章比每章少几百字糟糕得多。
inline constexpr std::size_t kAnalyzeBudgetChars = 12000;

/// 每章至少给这么多字，再多章也不能少于它。
inline constexpr std::size_t kAnalyzeMinPerChapter = 200;

}  // namespace changji::stages::prompt
