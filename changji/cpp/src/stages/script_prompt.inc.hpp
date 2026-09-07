// 本文件由 cpp/tools/gen_prompts.py 生成，不要手改。
// 剧本、选题、预告片三个提示词。都是行 join 出来的，带条件块，
// 所以分两层切：Seg/Rules/Tail 是必然出现的，Block 是可选的。
// 拼接顺序见下面的注释。

#pragma once

namespace changji::stages::prompt {

// 正片：Seg0 + 时长 + Seg1 + 画风 + Seg2 + 字数 + Rules
//       + [CharsPre + 角色名 + CharsPost]
//       + [PrevPre + 前情 + PrevPost]
//       + TailHead + 梗概 + TailEnd
// 选题：Seg0 + 画风 + Seg1 + 数量 + Rules
//       + [KeywordsPre + 关键词 + KeywordsPost]
//       + [ExistingPre + 已有方向 + ExistingPost]
//       + TailEnd
// 预告片：Seg0 + 画风 + Seg1 + 时长 + Seg2 + 字数 + Rules
//         + [CharsPre + 角色名 + CharsPost]
//         + [EpisodesPre + 已写剧集 + EpisodesPost]
//         + TailHead + 梗概 + TailEnd

inline constexpr const char* kScriptSeg0 =
    R"CJ(你在写一集竖屏短剧，总时长约 )CJ";

inline constexpr const char* kScriptHintRealistic =
    R"CJ(真人写实短剧，台词生活化，不要文绉绉的书面语)CJ";

inline constexpr const char* kScriptHintAnime =
    R"CJ(动漫短剧，台词可以更有戏剧张力，但仍要口语化)CJ";

inline constexpr const char* kScriptSeg1 =
    R"CJ( 秒。)CJ";

inline constexpr const char* kScriptSeg2 =
    R"CJ(。

硬性要求：
1. 所有对白加起来控制在 )CJ";

inline constexpr const char* kScriptRules =
    R"CJ( 个字左右，超出很多就是拍不完。
2. 动作和对白交替推进，不要连着好几句对白，画面会没有呼吸。
3. 出场角色不超过三个。人一多，短剧里根本立不住。
4. 对白只写说出口的话，不要带引号，不要在 text 里重复人名。
5. 同一个角色的名字前后必须一模一样，不要一会儿全名一会儿简称。
6. 开头三秒就要有事发生，短剧没有铺垫的余地。
7. 结尾留一个钩子或者一个明确的情绪落点。

)CJ";

inline constexpr const char* kScriptCharsPre =
    R"CJ(必须沿用这些已有角色，名字一字不改：)CJ";

inline constexpr const char* kScriptCharsPost =
    R"CJ(。

)CJ";

inline constexpr const char* kScriptPrevPre =
    R"CJ(前面几集的剧本如下，这一集要接着往下写，人物关系和已经发生的事不能推翻：

)CJ";

inline constexpr const char* kScriptPrevPost =
    R"CJ(

)CJ";

inline constexpr const char* kScriptTailHead =
    R"CJ(这一集要写的：

)CJ";

inline constexpr const char* kScriptTailEnd =
    R"CJ(

只输出 JSON，不要任何解释文字。)CJ";

inline constexpr const char* kPremiseSeg0 =
    R"CJ(你在给一部竖屏)CJ";

inline constexpr const char* kPremiseHintRealistic =
    R"CJ(真人写实短剧，题材要落在现实生活里)CJ";

inline constexpr const char* kPremiseHintAnime =
    R"CJ(动漫短剧，可以有超现实设定，但情感冲突要真实)CJ";

inline constexpr const char* kPremiseSeg1 =
    R"CJ(想选题。给出 )CJ";

inline constexpr const char* kPremiseRules =
    R"CJ( 个不同方向的方案。

硬性要求：
1. premise 必须具体到人物和处境，一两句话。「都市复仇爽剧」这种是题材标签不是选题，不要给。
2. 一句话里就要有冲突。没有冲突的设定拍不成短剧。
3. 主要人物不超过三个，短剧里人一多就立不住。
4. 几个方案要拉开差距，不要三个都是同一个故事换名字。
5. 是能一直往下拍的设定，不是一集就讲完的段子。
6. title 是剧名，八个字以内，不要副标题。

)CJ";

inline constexpr const char* kPremiseKeywordsPre =
    R"CJ(往这个方向想：)CJ";

inline constexpr const char* kPremiseKeywordsPost =
    R"CJ(

)CJ";

inline constexpr const char* kPremiseExistingPre =
    R"CJ(下面这些方向已经有了，换别的：
)CJ";

inline constexpr const char* kPremiseExistingPost =
    R"CJ(

)CJ";

inline constexpr const char* kPremiseTailEnd =
    R"CJ(只输出 JSON，不要任何解释文字。)CJ";

inline constexpr const char* kTrailerSeg0 =
    R"CJ(你在写一部竖屏)CJ";

inline constexpr const char* kTrailerHintRealistic =
    R"CJ(真人写实)CJ";

inline constexpr const char* kTrailerHintAnime =
    R"CJ(动漫)CJ";

inline constexpr const char* kTrailerSeg1 =
    R"CJ(短剧的预告片，总时长约 )CJ";

inline constexpr const char* kTrailerSeg2 =
    R"CJ( 秒。

预告片不是把正片缩短，是另一种东西。硬性要求：
1. 所有对白加起来控制在 )CJ";

inline constexpr const char* kTrailerRules =
    R"CJ( 个字以内。预告片以画面为主，话越少越有劲。
2. 用蒙太奇：几个不相干的瞬间快速切换，不要讲一条完整的时间线。
3. 前两秒必须是全片最抓人的那个画面或那句话，刷到就得停下来。
4. 只给钩子，不给答案。关键情节点到为止，结局绝对不能剧透。
5. 结尾停在悬念上，可以是一句反问、一个未完成的动作或一个眼神。
6. 对白只写说出口的话，不要带引号，不要在 text 里重复人名。
7. 出场角色不超过三个，名字前后一模一样。
8. title 写这部剧的名字，logline 写一句能当封面文案的钩子。

)CJ";

inline constexpr const char* kTrailerCharsPre =
    R"CJ(必须沿用这些已有角色，名字一字不改：)CJ";

inline constexpr const char* kTrailerCharsPost =
    R"CJ(。

)CJ";

inline constexpr const char* kTrailerEpisodesPre =
    R"CJ(已经写好的剧集如下。从里面挑最有冲击力的瞬间来剪，不要编造剧里没有的情节，也不要把结局说出来：

)CJ";

inline constexpr const char* kTrailerEpisodesPost =
    R"CJ(

)CJ";

inline constexpr const char* kTrailerTailHead =
    R"CJ(这部剧讲的是：

)CJ";

inline constexpr const char* kTrailerTailEnd =
    R"CJ(

只输出 JSON，不要任何解释文字。)CJ";

// 说话人为空的各种写法。模型经常无视 schema 填 none、旁白 这类词，
// 原样当名字用的话，成片字幕上会出现「none：寂静」。
inline constexpr const char* kNoSpeaker[] = {
    R"CJ((none))CJ",
    R"CJ(-)CJ",
    R"CJ(n/a)CJ",
    R"CJ(na)CJ",
    R"CJ(narrator)CJ",
    R"CJ(nil)CJ",
    R"CJ(none)CJ",
    R"CJ(none.)CJ",
    R"CJ(null)CJ",
    R"CJ(ost)CJ",
    R"CJ(vo)CJ",
    R"CJ(voiceover)CJ",
    R"CJ(—)CJ",
    R"CJ(旁白)CJ",
    R"CJ(无)CJ",
    R"CJ(画外音)CJ",
    R"CJ(空)CJ",
    R"CJ(（无）)CJ",
};

// 整段外面套的括号和引号。左右相同的（引号）判断规则不一样，
// 见 C++ 侧 wraps_whole 的注释。
inline constexpr const char* kWrappers[][2] = {
    {R"CJ(（)CJ", R"CJ(）)CJ"},
    {R"CJ(()CJ", R"CJ())CJ"},
    {R"CJ(【)CJ", R"CJ(】)CJ"},
    {R"CJ([)CJ", R"CJ(])CJ"},
    {R"CJ(“)CJ", R"CJ(”)CJ"},
    {R"CJ(")CJ", R"CJ(")CJ"},
    {R"CJ(「)CJ", R"CJ(」)CJ"},
    {R"CJ(『)CJ", R"CJ(』)CJ"},
    {R"CJ(')CJ", R"CJ(')CJ"},
};

// 只削开头这几种括号里的时间码。
inline constexpr const char* kLeadBrackets[][2] = {
    {R"CJ([)CJ", R"CJ(])CJ"},
    {R"CJ(【)CJ", R"CJ(】)CJ"},
    {R"CJ(（)CJ", R"CJ(）)CJ"},
    {R"CJ(()CJ", R"CJ())CJ"},
};

// 括号里出现这些才算时间码。光有数字不够——
// 「（他犹豫了3秒）」和「（第3次）」得区分开。
inline constexpr const char* kTimeUnits[] = {
    R"CJ(秒)CJ",
    R"CJ(s)CJ",
    R"CJ(S)CJ",
    R"CJ(:)CJ",
    R"CJ(：)CJ",
    R"CJ(分)CJ",
    R"CJ(帧)CJ",
};

// 一集里对白占的比重。剩下的是动作和环境描写，
// 不发声但占画面时长。全是对白的话成片像念稿子。
inline constexpr double kDialogueShare = 0.62;
inline constexpr double kCharsPerSecond = 4.6;

// 塞进提示词前要截断的长度，按**字符**不是字节。
inline constexpr std::size_t kPrevMaxChars = 4000;
inline constexpr std::size_t kEpisodesMaxChars = 6000;
inline constexpr std::size_t kExistingMaxChars = 60;
inline constexpr std::size_t kExistingMaxItems = 6;

}  // namespace changji::stages::prompt
