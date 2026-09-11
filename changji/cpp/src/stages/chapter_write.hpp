#pragma once

// 逐章展开正文。故事分三层里的最后一层。
//
//     梗概 → 故事大纲 → **章节正文**
//
// 补的是 AI 那条路上最后一个洞：大纲写出来的故事只有「这一章发生什么，
// 三五句」，没有正文。于是写剧本时展开的是梗概，而按字符切分那套机器
// （episode_text）一直返回空。粘贴进来的故事天生有正文，走不到这一步。
//
// 一次只写一章，每次带的是**压缩的全局记忆 + 上一章结尾**，不是前面所有
// 章的正文——那和逐集续写的失忆是同一个道理，二十章的正文谁也塞不下。
//
// 纯函数，不碰网络也不碰 llama.cpp。

#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "models/character.hpp"
#include "models/story.hpp"

namespace changji::stages {

/// 一个钩子：这一章里可以收一集的地方。
struct DraftHook {
    std::string text;  ///< 这里悬着的是什么
    /// 这个位置前面那句原文，照抄十到二十个字。程序靠它在正文里定位。
    ///
    /// 让模型报字符偏移是行不通的——它数不准，报出来的数会落在别的段落上。
    /// 让它抄一句原文，程序自己去查，这是唯一可靠的定位手段。
    std::string after;
};

/// 模型写回来的一场戏。
///
/// **场是 2026-09-12 加的写作单位**，见 models::Scene 上那段注释：没有它
/// 的时候模型把整章梗概平摊成四十个一句话的段落，通篇是概述不是场景。
struct DraftScene {
    std::string where;
    std::string pov;
    std::string goal;
    std::string obstacle;
    std::string worse;             ///< 这一场收场时局面比开场时更糟在哪儿
    std::string turn;              ///< 这一场结束时局面变成什么。就是这一集的钩子
    std::vector<std::string> paragraphs; ///< 这一场的正文，一段一项
};

/// 模型写回来的一章。
struct ChapterDraft {
    std::string text;
    /// 这一章的场次，按先后。正文就是把它们的段落顺次拼起来的。
    std::vector<DraftScene> scenes;
    /// 这一章里所有可以收一集的地方，按先后。
    ///
    /// **场次表出现之后，这一栏只剩下兼容老形状的用处**：切点现在由场的
    /// 末尾直接给出（程序自己数得出位置），不再靠模型抄一句原文回来定位。
    /// 老草稿、老 schema 回来的 hooks 还认。
    std::vector<DraftHook> hooks;
};

/// 章节正文在模型那份 JSON 里的字段名。
///
/// **不能各写各的。** 这个名字有两个读者：`chapter_schema()` 写进 schema，
/// 而流式那一层（`stages::JsonFieldStreamer`）要照着它从 token 流里把正文
/// 抠出来推给编辑器。2026-09-11 就栽在这儿——schema 从 `text` 改成
/// `paragraphs`，流式那边没跟着改，于是它一个字都抠不出来：编辑器整整
/// 一两分钟一动不动，而**后端不报任何错**（正文照样解析、落库、重算分集），
/// 查起来毫无线索。
///
/// 现在两边都用这一个常量，再配一条用例钉住它确实是 schema 里的键。
///
/// ⚠️ 2026-09-12 起这个键**在每一场里各有一个**（`scenes[].paragraphs`），
/// 不再是顶层唯一的那一个。流式那一层因此要开「一路收到底」那个开关
/// （`JsonFieldStreamer` 的 repeating），否则它收完第一场就 done()，
/// 编辑器里只看得到三分之一——而后端照样不报错。
inline constexpr const char* kChapterBodyField = "paragraphs";

/// 场次表在模型那份 JSON 里的字段名。
inline constexpr const char* kChapterScenesField = "scenes";

/// 一章的基准篇幅。
///
/// **一章是一个完整的故事单元，不是一集。** 三千字是网文一章的常见体量，
/// 也和粘贴导入那边按字数切章用的 kImportTargetChars 对齐——AI 写的章和
/// 人粘进来的章该是同一个数量级，不然同一部剧里两种来源的章长得不一样。
inline constexpr int kChapterTargetChars = 3000;

/// 一章至少要切得出这么多集。
///
/// 光有上面那个基准不够：每集选 180 秒时一集能吃 2700 字，三千字的章又变成
/// 一章一集了。两个取大的，保证**在任何每集时长下，一章都跨好几集**。
inline constexpr int kEpisodesPerChapter = 3;

/// 这一章该写多长。
///
/// **章的篇幅由故事本身定，和每集多长无关**——每集多长只决定这一章切成
/// 几集。早先这里是反过来的：按「它要撑起几集 × 每集容量」算，而大纲阶段
/// 一章一集，算出来永远是一集的量，于是分集算法的活（把章切成集）等于
/// 没做。端到端实跑时露的馅：四章写出来 340/621/399/371 字，切出来正好
/// 四集。
int chapter_target_chars(const models::Story& story);

/// 一段平均多少字。**注意是平均数，不是中位数。**
///
/// 2026-09-11 量过起点主流长篇（《诡秘之主》第 2~6 章）：段长**中位** 33 字，
/// 八成以上的段只有一句话——但那是中位数，真实的一章里还夹着上百字的长段，
/// 平均数比中位数高一截。
///
/// 上一版拿 33 那个中位数当平均数用（写的是 35），段数 = 字数/35 进了语法，
/// 于是「每段三十五字」成了硬指标：模型只能一段一句、句句等长，写出来是
/// 字幕不是小说。取 45，段长才有长有短的余地。
inline constexpr int kCharsPerParagraph = 45;

/// 一场戏最少写多少字。
///
/// 一场要铺得开「谁想干什么、谁拦着、局面变成什么」，还要有对白往返和
/// 感官细节。低于这个数就又被压回概述了。
inline constexpr int kSceneMinChars = 600;

/// 一场戏最多写多少字。再长的话一场要横跨好几集，切点只能落在场中间。
inline constexpr int kSceneMaxChars = 1500;

/// 一场戏该写多少字。**跟着每集时长走。**
///
/// 写死 1000 的时候，每集 30 秒（一集吃 450 字）那一档里一场横跨快两集，
/// 分集只能在场中间下刀——2026-09-12 实跑，停在场尾从 86% 掉到 63%。
/// 一场对一集是最顺的形状：那一集的结尾正好是这场戏演完的地方。
///
/// 夹在 600 和 1500 之间：短了写不成一场戏，长了一场横跨好几集，两头
/// 都回到「切在场中间」。行业上一集 1~3 分钟是主流，那一档正好落在中间。
int scene_target_chars(const models::Story& story);

/// 这一章写成几场戏。**按字数算，不按它会被切成几集算。**
///
/// 第一版是按集数算的（一场一集，切点最整齐）。2026-09-12 实跑当场露馅：
/// 每集 30 秒时一章要 4 场，而一章的梗概只撑得起一两件事——模型就从全局
/// 地点表里抓了下一章的地方来凑，四章都在同一个楼顶演同一件事。
///
/// 夹在 2 和 4 之间：一场都没有就退回了上一版那种平摊；四场以上每场分不到
/// 一千字，又会被压回概述。
int chapter_target_scenes(const models::Story& story);

/// 一场戏写多少字。
int chapter_scene_chars(const models::Story& story);

/// 一场戏写多少段。从每场字数算，进 schema 卡住。
int chapter_scene_paras(const models::Story& story);

/// 请求里带的 JSON Schema。
///
/// **形状是 scenes[]，每一场自己带 paragraphs**。本地后端把 schema 转成
/// GBNF 硬约束，远端靠 response_format。所以「一章分几场」「一场写多少段」
/// 不是提示词里的一句请求，而是语法层面的下限和上限——2026-09-11 的教训是
/// 措辞它不听，schema 它没得选（正文还是一个 text 字符串时，14B 三章里两章
/// 把梗概原样抄进去就收工）。
///
/// 每一场的 where/pov/goal/obstacle 排在 paragraphs **前面**是有意的：
/// 模型是顺着往下生成的，先把这一场在哪、跟谁走、要什么、谁拦着填掉，
/// 后面那几百字才有地方落。turn 也排在前面——先知道这一场停在哪，才写得到
/// 那儿去（「先想好在哪儿断，再写到那儿」）。
nlohmann::ordered_json chapter_schema(int target_scenes, int paras_per_scene);

/// 拼提示词。chapter_id 不存在时抛。
std::string build_chapter_prompt(const models::Story& story,
                                 const std::string& chapter_id,
                                 models::StyleLine style_line);

/// 正文至少要有目标篇幅的这么多，否则算模型没写。
///
/// 五分之一给得很松——**要拦的是"根本没写"，不是"写短了"**。
/// 2026-09-11 实跑时模型把章标题填进了正文字段，四章各写出 1~2 个字，
/// 而这些**被静默存了下来**：故事看着有四章，分集只切出一集，
/// 到写剧本那一步才会发现无米下锅。和剧本那边「整集一句台词都没有」
/// 是同一类闸门——宁可报错重来，不要留一份看着正常的空壳。
inline constexpr double kChapterMinRatio = 0.2;

/// 这两句话是不是在说同一件事。
///
/// 两条任一成立：去标点之后互相包含、十个字以上连着一样。
///
/// 「换个说法的同一件事」抓不到——量过了，用词重合度在真撞车和真不同
/// 之间分不开（0.30 对 0.27），那个信号在语义里不在字面里。
///
/// 分集的钩子就是场的 turn，两集钩子撞车观众看到的是剧情在原地打转。
/// 导出来是因为「这一章和上一章停在同一件事上」那道闸在 apply_chapter 里，
/// 而它和 parse_chapter 里章内那道用的必须是同一把尺子。
bool scenes_repeat_beat(const std::string& a, const std::string& b);

/// 解析模型返回。
///
/// `min_chars` 给 0 表示不查长度（拼提示词的单测用得着）。
///
/// `strict` = 软闸开不开。**闸分两档，这是 2026-09-12 用三轮实跑换来的：**
///
/// - **硬闸**（没写出正文、复读、模型的自言自语、英文占位符、短得离谱）
///   任何时候都拦。它们拦下来的东西**没法用**——落库就是一章垃圾。
/// - **软闸**（整章没对白、情绪标签成串、两场停在同一件事、章尾点题）
///   只在还有下一次机会时拦。它们拦下来的东西是**能用但不够好**。
///
/// 分档的理由很实在：软闸一律硬拦的话，14B 连着两次写不对，那一章就是
/// **0 字**——而 0 字比「写得一般」差得多。三轮实跑里软闸每轮清掉 1~3 章，
/// 把想提的分自己打没了。最后一次尝试放它过去，软闸就只会提分，不会清零。
ChapterDraft parse_chapter(const std::string& raw, int min_chars = 0,
                           bool strict = true);

/// 把写好的一章并回故事里。
///
/// **只动这一章。** 正文落进去之后钩子要重建：段落边界重新登记成候选切点
/// （原来那些是对着空正文算的，全都作废），再把**每一场的末尾**登记成有说法
/// 的切点——说法就是那一场的 turn。大纲里那一章的钩子说法留着挂在章尾。
///
/// 场的位置是**程序数出来的**：正文是把各场的段落顺次拼起来的，第几段结束
/// 就是第几场结束，一个字都不用模型报。上一版靠模型抄一句原文回来查位置
/// （DraftHook::after），抄错一个字那一集就落不下去。
models::Story apply_chapter(const models::Story& story,
                            const std::string& chapter_id,
                            const ChapterDraft& draft);

}  // namespace changji::stages
