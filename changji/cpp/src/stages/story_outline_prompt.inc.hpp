// 故事大纲的提示词。**这个文件就是出处，直接改它。**
//
// 和 script_prompt / bible_prompt 那几个不一样：那些是从已经删掉的 Python
// 引擎移植过来的，要求逐字节一致（有对拍语料钉着）。这一份是新写的，
// 没有 Python 对应物，**不受逐字节约束**，该调就调。
//
// 拼接顺序：
//   Seg0 + 画风 + Seg1 + 章数 + Seg2 + Rules
//        + [KeywordsPre + 关键词 + KeywordsPost]
//        + TailHead + 梗概 + TailEnd

#pragma once

namespace changji::stages::prompt {

inline constexpr const char* kOutlineSeg0 =
    R"CJ(你在给一部竖屏)CJ";

inline constexpr const char* kOutlineHintRealistic =
    R"CJ(真人写实短剧)CJ";

inline constexpr const char* kOutlineHintAnime =
    R"CJ(动漫短剧)CJ";

inline constexpr const char* kOutlineSeg1 =
    R"CJ(写故事大纲。整个故事写成 )CJ";

inline constexpr const char* kOutlineSeg2 =
    R"CJ( 章左右。

硬性要求：
)CJ";

inline constexpr const char* kOutlineRules =
    R"CJ(1. 这是一个**有结尾的完整故事**，不是一个能一直往下拍的设定。
   最后一章要把主线了结，不要留「待续」。
2. 章数按故事本身的需要来。上面那个数只是量级，多一两章少一两章都行，
   但不要为了凑数注水，也不要硬收。
3. 每一章只推进一件事。一章里塞三条线，后面按时长切集时切不开。
   但**一章是一个完整的故事单元，不是一句话**：它后面要展开成几千字正文，
   再按每集时长切成好几集。所以 summary 要写清这一章从哪儿开始、中间
   发生什么、到哪儿为止，撑得起这个体量。
4. 每一章的 hook 是**这一章结束时悬着的那件事**：一个悬念、一个反转，
   或者一个明确的情绪落点。后面每一集的结尾都要落在这种位置上，
   所以每章都必须有，不能空着。
5. 主要人物不超过三个。短剧里人一多就立不住。配角只写真的有戏的。
6. **不要写任何长相、发型、服装、身材。** 人物长什么样由后面的美术那一步
   统一定，这里写了两边必然不一致，而不一致会一路带到每一个镜头上。
   这里只写他是谁、他要什么、他怎么变。
7. 每一组有戏的关系都要写进 relations，并且说清那段关系里绷着的是什么。
   只写「前任」不够，要写成「谁欠谁一句没说出口的道歉」。
8. want 是这个人物**主动要的东西**。没有欲望的人物推不动情节，
   写「希望生活变好」这种等于没写。
9. 地点要具体到看得见：不说「一家咖啡馆」，说「高架桥下那家通宵咖啡馆」。
   后面每一镜都要照着它画，含糊的地方每镜长得都不一样。
10. 人物名前后一模一样，不要一会儿全名一会儿简称。

)CJ";

inline constexpr const char* kOutlineKeywordsPre =
    R"CJ(往这个方向想：)CJ";

inline constexpr const char* kOutlineKeywordsPost =
    R"CJ(

)CJ";

inline constexpr const char* kOutlineTailHead =
    R"CJ(这部剧讲的是：

)CJ";

// 没给梗概时走这一段。
//
// **梗概不是必填的。** 三个入口里只有「我自己有个想法」那条是从手写的
// 一句话开始的；「给几个关键词」和「什么都没有，你来一个」同样正当，
// 而把梗概做成硬门槛等于又把人摁回空白框前面发呆。选题本来就是最难
// 从零开始的一步。
inline constexpr const char* kOutlineNoPremise =
    R"CJ(这部剧讲什么**由你定**。先自己想一个选题，再照着它写大纲。
选题要具体到人物和处境，一句话里就得有冲突——「都市复仇爽剧」那种是
题材标签不是选题。把它写进 premise 那一项。

)CJ";

inline constexpr const char* kOutlineTailEnd =
    R"CJ(

只输出 JSON，不要任何解释文字。)CJ";

// 塞进提示词前要截断的长度，按**字符**不是字节。
inline constexpr std::size_t kOutlinePremiseMaxChars = 2000;
inline constexpr std::size_t kOutlineKeywordsMaxChars = 200;

// 模型吐得离谱时的上限。不拦的话一份大纲能撑爆后面每一步的上下文。
inline constexpr std::size_t kMaxChapters = 200;
inline constexpr std::size_t kMaxCharacters = 60;

}  // namespace changji::stages::prompt
