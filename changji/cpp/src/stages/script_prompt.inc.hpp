// **这个文件现在就是提示词的出处，直接改它。**
//
// 原来这里写的是"由 cpp/tools/gen_prompts.py 生成，不要手改"。那个生成器
// 2026-09-10 随 Python 引擎一起删了（见 6d6982a），禁令却留了下来——
// 照着它去找生成器会找不到，而不敢改这个文件就等于提示词冻死在这儿。
// 剧本、选题、预告片三个提示词。都是行 join 出来的，带条件块，
// 所以分两层切：Seg/Rules/Tail 是必然出现的，Block 是可选的。
// 拼接顺序见下面的注释。

#pragma once

namespace changji::stages::prompt {

// 正片：Seg0 + 时长 + Seg1 + 画风 + Seg2 + 字数 + Rules
//       + 四段（ActBlockHead + 每段一行 + ActBlockTail，见 render_act_brief）
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

动作那几拍要**拍得出来**。下一步要照着它画分镜，所以：
8. 换地方、换时间就在那一拍的开头交代清楚：在哪、什么时候、什么光。
   例如「深夜，天台，只有应急灯」。同一个地方连着几拍就不用重复。
9. 写角色**身体在做什么**，别写他心里怎么想。
   不要「她很生气」，要「她把茶杯重重放下，茶水溅到手背上」。
   情绪从动作和台词里透出来，写心理活动画不出来。
10. 提到的东西要具体到看得见：不说「一件信物」，说「一枚缺了角的铜钱」。
    后面每一镜都要拿它当道具，含糊的东西每镜长得都不一样。
11. 每一拍只写一件事。一拍里塞三个动作，分镜只能挑一个画，剩下的就丢了。

)CJ";

// ---- 四段 ----
//
// 一集按秒排成四段，正片和从故事写的那条路共用。**这一段必须进提示词**，
// 光写在 schema 的 description 里模型看不见（GBNF 只有结构）。
// 每段那一行由 render_act_brief 拼：「  开场钩子（0–5 秒）：<brief>。至少 N 拍。」

inline constexpr const char* kActKeys[4] = {"opening", "escalation", "payoff",
                                             "cliff"};
inline constexpr const char* kActLabels[4] = {
    R"CJ(开场钩子)CJ", R"CJ(冲突推进)CJ", R"CJ(情绪回报)CJ", R"CJ(集尾留扣)CJ"};
inline constexpr const char* kActBriefs[4] = {
    R"CJ(一句冲突台词或一个反常画面，三秒内有事发生，不铺垫)CJ",
    R"CJ(对立方步步紧逼，一步比一步紧，中途给一次小回报再压回去)CJ",
    R"CJ(反击、打脸或真相——观众等的那一下)CJ",
    R"CJ(在情绪最高或最意外处切断，最后一拍就是钩子)CJ",
};

inline constexpr const char* kActBlockHead =
    R"CJ(这一集分四段，按秒排。JSON 里 opening、escalation、payoff、cliff 四项就是它们，顺序不能换：
)CJ";

inline constexpr const char* kActBlockTail =
    R"CJ(一拍是一个动作或一句台词。四段加起来要撑满整集，每段的拍数照上面的下限往上写——写少了这一集就只有十几秒。

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

// 机位标签里的景别词。动作行开头挂一个「镜头特写：」时靠它认出来。
//
// **不进提示词，只在解析时用。** 提示词里列一串「不要写镜头/特写/近景」，
// 模型会把这些词原样抄进正文——2026-09-11 在章节那边栽过一次
// （反例句被逐字抄走）。形式是我们定的，削掉就完了。
inline constexpr const char* kCameraWords[] = {
    R"CJ(镜头)CJ",
    R"CJ(特写)CJ",
    R"CJ(近景)CJ",
    R"CJ(中景)CJ",
    R"CJ(远景)CJ",
    R"CJ(全景)CJ",
    R"CJ(空镜)CJ",
    R"CJ(画面)CJ",
    R"CJ(闪回)CJ",
    R"CJ(插入)CJ",
    // 2026-09-13 补的一批**后期/转场**术语。上面那十个都是「怎么拍」，
    // 这一批是「不是拍出来的」——行业写法里这类信息本来就用【】标出来，
    // 意思是"画面里没有，是后期合成的"。模型会把它们写成 `标签：内容`，
    // 而那和一句台词长得一模一样。
    //
    // **实跑撞上的**（预告片那条路）：
    //     黑屏前最后一帧：林浩抬头望向镜头，雨水顺着脸颊滑落……
    // 冒号前七个字，script_dialogue_pairs 认「冒号前 ≤12 字 = 说话人」，
    // 于是整句动作描写变成一个叫「黑屏前最后一帧」的人在说话；名字认不出
    // 就落成旁白，**旁白音会把它念出来**。
    //
    // 只收确定不会被念出口的那些。**不收「字幕」**：「字幕：三年后」削成
    // 「三年后」之后，让旁白念一句"三年后"其实是正当的转场处理，
    // 两种做法都说得通，不该在这一层替人决定。
    R"CJ(黑屏)CJ",
    R"CJ(定格)CJ",
    R"CJ(定场)CJ",
    R"CJ(淡入)CJ",
    R"CJ(淡出)CJ",
    R"CJ(化入)CJ",
    R"CJ(叠化)CJ",
    R"CJ(转场)CJ",
    R"CJ(航拍)CJ",
    R"CJ(慢镜)CJ",
    R"CJ(特效)CJ",
    R"CJ(蒙太奇)CJ",
};

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
