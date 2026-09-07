"""从 Python 的提示词函数里抽出模板，生成 C++ 的原始字符串字面量。

跑法（在 changji/ 目录下）：
    .venv/Scripts/python.exe cpp/tools/gen_prompts.py

产出：
    cpp/src/stages/bible_prompt.inc.hpp
    cpp/src/stages/storyboard_prompt.inc.hpp
    cpp/src/stages/shot_schema.inc.hpp

改了 Python 侧的提示词或 Shot 模型之后要重跑，然后重新生成对拍语料
（cpp/tests/export_bible_golden.py、export_storyboard_golden.py）。

手抄这些文字是不可接受的：几百个汉字里错一个，提示词就和 Python 不一致了，
而这类差异不会报错，只会让模型的输出悄悄变一点，要跑几十个镜头才看得出来。
Shot 的 schema 更甚——那是 pydantic 从模型定义生成的，手写一份必然随
模型演进而腐烂，而腐烂的方式是"模型按旧字段名产出，解析阶段拿到一堆空值"。
"""
import io
import json
import sys

sys.path.insert(0, "src")  # 从 changji/ 跑

from changji.models.character import (                            # noqa: E402
    AppearanceBlock, AssetLibrary, Character, Location, StyleLine,
)
from changji.models.shot import Shot                              # noqa: E402
from changji.stages.bible import build_prompt as bible_prompt     # noqa: E402
from changji.stages import script as sc                           # noqa: E402
from changji.stages import storyboard as sb                       # noqa: E402

DELIM = "CJ"


def lit(s: str) -> str:
    """转成 C++ 原始字符串字面量。"""
    assert ")" + DELIM + '"' not in s, "分隔符撞车了，换一个"
    return 'R"' + DELIM + "(" + s + ")" + DELIM + '"'


def header(lines: list[str]) -> str:
    out = io.StringIO()
    out.write("// 本文件由 cpp/tools/gen_prompts.py 生成，不要手改。\n")
    for line in lines:
        out.write("// " + line + "\n")
    out.write("\n#pragma once\n\nnamespace changji::stages::prompt {\n\n")
    return out.getvalue()


def split_by(text: str, marks: list[str]) -> list[str]:
    """按一串哨兵切开，返回 len(marks)+1 段。"""
    parts = []
    rest = text
    for m in marks:
        assert m in rest, f"哨兵 {m!r} 没出现在提示词里，Python 侧的模板可能改了"
        head, rest = rest.split(m, 1)
        parts.append(head)
    parts.append(rest)
    return parts


# ---- 角色圣经 ----

def gen_bible() -> None:
    MARK = "\x00SCRIPT\x00"
    real = bible_prompt(MARK, StyleLine.REALISTIC)
    anime = bible_prompt(MARK, StyleLine.ANIME)

    # 两者只有 style_hint 那一句不同，找出公共前后缀
    i = 0
    while real[i] == anime[i]:
        i += 1
    j = 0
    while real[-1 - j] == anime[-1 - j]:
        j += 1

    prefix, real_hint = real[:i], real[i:len(real) - j]
    anime_hint = anime[i:len(anime) - j]
    mid, tail = real[len(real) - j:].split(MARK)

    out = io.StringIO()
    out.write(header(["提示词要求和 Python 逐字节一致。"]))
    for name, val in [("kBiblePrefix", prefix),
                      ("kBibleHintRealistic", real_hint),
                      ("kBibleHintAnime", anime_hint),
                      ("kBibleMiddle", mid),
                      ("kBibleTail", tail)]:
        out.write(f"inline constexpr const char* {name} =\n    {lit(val)};\n\n")
    out.write("}  // namespace changji::stages::prompt\n")

    path = "cpp/src/stages/bible_prompt.inc.hpp"
    io.open(path, "w", encoding="utf-8", newline="\n").write(out.getvalue())
    print("写好了", path)


# ---- 分镜 ----

def _probe_assets() -> AssetLibrary:
    """给 build_prompt 用的最小资产库。只用来定位插值点，内容无所谓。"""
    return AssetLibrary(
        characters={"c_probe": Character(
            char_id="c_probe", name="占位",
            appearance=AppearanceBlock(identity="占位", face="占位", attire="占位"))},
        locations={"loc_probe": Location(
            location_id="loc_probe", name="占位", space="占位", lighting="占位")},
    )


def gen_storyboard() -> None:
    # 每个插值点插一个独一无二的哨兵，然后按哨兵切开。
    # 哨兵用 \x00 包着，正常文本里不可能出现。
    ROSTER = "\x00ROSTER\x00"
    PLACES = "\x00PLACES\x00"
    DESCRIBE = "\x00DESCRIBE\x00"
    COUNT = "\x00COUNT\x00"
    TOTAL = "\x00TOTAL\x00"
    EPISODE = "\x00EPISODE\x00"
    SCRIPT = "\x00SCRIPT\x00"

    class FakeQuota:
        """假配额，只为了让哨兵原样出现在提示词里。

        真配额的 describe() 返回中文，total_s 是数字——那些值本身
        没法当哨兵用，所以整个替换掉。
        """
        def describe(self):
            return DESCRIBE

        @property
        def shot_count(self):
            return COUNT

        @property
        def total_s(self):
            return TOTAL

    # total_s 走的是 f"{...:g}"，得让它对字符串也能格式化
    class GFormat(str):
        def __format__(self, spec):
            return str(self)

    class FakeQuota2(FakeQuota):
        @property
        def shot_count(self):
            return GFormat(COUNT)

        @property
        def total_s(self):
            return GFormat(TOTAL)

    assets = _probe_assets()
    text = sb.build_prompt(SCRIPT, assets, FakeQuota2(), EPISODE)

    # roster 和 places 是 build_prompt 自己从 assets 拼的，换成哨兵
    real_roster = "  c_probe：占位"
    real_places = "  loc_probe：占位"
    assert real_roster in text and real_places in text, "名单的拼法变了"
    text = text.replace(real_roster, ROSTER).replace(real_places, PLACES)

    parts = split_by(text, [ROSTER, PLACES, DESCRIBE, COUNT, TOTAL,
                            EPISODE, SCRIPT])
    names = ["kSbSeg0", "kSbSeg1", "kSbSeg2", "kSbSeg3", "kSbSeg4",
             "kSbSeg5", "kSbSeg6", "kSbSeg7"]
    assert len(parts) == len(names)

    out = io.StringIO()
    out.write(header([
        "分镜提示词，按插值点切成八段。拼的顺序是：",
        "  seg0 + 角色名单 + seg1 + 场景名单 + seg2 + 配额描述 +",
        "  seg3 + 镜头数 + seg4 + 总时长 + seg5 + 剧集 id + seg6 + 剧本 + seg7",
    ]))
    for name, val in zip(names, parts):
        out.write(f"inline constexpr const char* {name} =\n    {lit(val)};\n\n")
    out.write("}  // namespace changji::stages::prompt\n")

    path = "cpp/src/stages/storyboard_prompt.inc.hpp"
    io.open(path, "w", encoding="utf-8", newline="\n").write(out.getvalue())
    print("写好了", path)


# ---- Shot 的 JSON Schema ----

def gen_shot_schema() -> None:
    """把 pydantic 生成的 Shot schema 原样嵌进 C++。

    不手写的理由和提示词一样，但更硬：这份 schema 是从 Shot 的字段定义
    自动导出的，手写一份必然随模型演进而腐烂。腐烂的表现不是编译错误，
    是模型按旧字段名产出、解析阶段拿到一堆空值——要跑一整集才发现。

    llm_shot_schema() 在这份基底上做的三件事（删运行时字段、把 id 收紧成
    枚举、去掉 needs_lipsync）在 C++ 里实现，因为那些依赖运行时的资产库。
    """
    schema = Shot.model_json_schema()
    # 紧凑但可读。这个串会被 nlohmann 在启动时解析一次，格式不影响结果。
    dumped = json.dumps(schema, ensure_ascii=False, indent=1, sort_keys=False)

    out = io.StringIO()
    out.write(header([
        "pydantic 从 Shot 的字段定义导出的 JSON Schema，原样嵌进来。",
        "llm_shot_schema() 在这上面做删字段和收紧枚举的加工。",
        "改了 Shot 的定义就要重跑这个脚本。",
    ]))
    out.write("inline constexpr const char* kShotSchemaJson =\n    "
              + lit(dumped) + ";\n\n")
    out.write("// 分镜阶段允许大模型填的字段。外观类字段不在其中，这是刻意的。\n")
    out.write("inline constexpr const char* kLlmShotFields[] = {\n")
    for f in sb._LLM_SHOT_FIELDS:
        out.write(f'    "{f}",\n')
    out.write("};\n\n")
    out.write("}  // namespace changji::stages::prompt\n")

    path = "cpp/src/stages/shot_schema.inc.hpp"
    io.open(path, "w", encoding="utf-8", newline="\n").write(out.getvalue())
    print("写好了", path, f"（schema {len(dumped)} 字节，"
                          f"{len(sb._LLM_SHOT_FIELDS)} 个允许字段）")


# ---- 剧本 / 选题 / 预告片 ----

def _common_edges(x: str, y: str):
    """两个串的公共前缀长度和公共后缀长度。"""
    i = 0
    while i < min(len(x), len(y)) and x[i] == y[i]:
        i += 1
    j = 0
    while j < min(len(x), len(y)) - i and x[-1 - j] == y[-1 - j]:
        j += 1
    return i, j


def _hint_split(real: str, anime: str, marks: list):
    """按哨兵切开两种画风的版本，把画风提示单独摘出来。

    不能像角色圣经那样直接对整串取公共前后缀——这三个提示词里画风提示的
    位置各不相同（正片在时长之后，选题和预告片在时长之前），
    整串取前缀会把哨兵一起吃进去。

    返回 (segs, hint_index, hint_real, hint_anime)。
    segs 比哨兵数多两段：出现画风提示的那一段被拆成了前后两半。
    """
    a = split_by(real, marks)
    b = split_by(anime, marks)
    assert len(a) == len(b)
    diff = [k for k in range(len(a)) if a[k] != b[k]]
    assert len(diff) == 1, "画风提示应该只影响一段，实际影响了 %d 段" % len(diff)
    k = diff[0]
    x, y = a[k], b[k]
    i, j = _common_edges(x, y)
    return (a[:k] + [x[:i], x[len(x) - j:]] + a[k + 1:], k,
            x[i:len(x) - j], y[i:len(y) - j])


def _optional_block(base: str, with_block: str, mark: str):
    """从"有这个块"和"没这个块"的两份里，把插入的那段摘出来。

    可选块的边界不在哨兵上（"必须沿用这些已有角色"那句和它前面的规则
    是连着的），所以只能靠 diff 定位。

    注意分隔用的两个换行落在**块的后半段**，不是前半段——Python 那边是
    `parts += ["", "必须沿用……"]`，那个空串贡献的换行在 join 之后跑到了
    前一项的尾巴上，于是 diff 出来的公共前缀把它吃掉了。所以拼接时是
    规则段(自带尾部换行) + Pre + 值 + Post，Post 里带着给下一块的换行。

    返回 (前半段, 后半段)。
    """
    i, j = _common_edges(base, with_block)
    inserted = with_block[i:len(with_block) - j]
    assert mark in inserted, "可选块里没有哨兵 %r，插入位置可能变了" % mark
    pre, post = inserted.split(mark, 1)
    return pre, post


def gen_script() -> None:
    """三个提示词构造器。

    和前两个不一样：这三个是把一串行 join 起来的，而且带条件块
    （有没有已有角色、有没有前几集、有没有关键词）。所以分两层切：
    先把必然出现的部分按哨兵切段，再把每个可选块单独 diff 出来，
    最后把"规则段"和"结尾段"从同一段里分开——可选块就插在它们中间。
    """
    DUR = "\x00DUR\x00"
    CHARS = "\x00CHARS\x00"
    NAMES = "\x00NAMES\x00"
    PREV = "\x00PREV\x00"
    PREMISE = "\x00PREMISE\x00"
    COUNT = "\x00COUNT\x00"
    KEYWORDS = "\x00KEYWORDS\x00"
    EXISTING = "\x00EXISTING\x00"
    EPISODES = "\x00EPISODES\x00"

    class GFloat(float):
        """让 f"{x:.0f}" 吐出哨兵而不是数字。"""
        def __new__(cls, mark):
            obj = super().__new__(cls, 1.0)
            obj.mark = mark
            return obj

        def __format__(self, spec):
            return self.mark

    real_budget = sc.budget_chars
    sc.budget_chars = lambda d: CHARS
    try:
        emit = []
        notes = []

        def add(name, val):
            emit.append((name, val))

        # ---------------- 正片 ----------------
        def script_of(style, previous="", characters=None):
            return sc.build_prompt(PREMISE, GFloat(DUR), style,
                                   previous=previous, characters=characters)

        base = script_of(StyleLine.REALISTIC)
        segs, k, hr, ha = _hint_split(base, script_of(StyleLine.ANIME),
                                      [DUR, CHARS, PREMISE])
        assert k == 1, "正片的画风提示应该在时长之后"
        # segs = [头, 时长后到提示前, 提示后, 规则+结尾头, 结尾尾]
        add("kScriptSeg0", segs[0])
        add("kScriptHintRealistic", hr)
        add("kScriptHintAnime", ha)
        add("kScriptSeg1", segs[1])
        add("kScriptSeg2", segs[2])

        cpre, cpost = _optional_block(
            base, script_of(StyleLine.REALISTIC, characters=[NAMES]), NAMES)
        ppre, ppost = _optional_block(
            base, script_of(StyleLine.REALISTIC, previous=PREV), PREV)

        # 结尾头（"这一集要写的：\n\n"）要从规则段里分出来，可选块插在中间。
        # 从"带前情"的版本里取：PREV 之后那一段是 ppost + 结尾头。
        wp = split_by(script_of(StyleLine.REALISTIC, previous=PREV),
                      [DUR, CHARS, PREV, PREMISE])
        assert wp[3].startswith(ppost)
        tail_head = wp[3][len(ppost):]
        rules = segs[3][:len(segs[3]) - len(tail_head)]
        assert segs[3] == rules + tail_head

        add("kScriptRules", rules)
        add("kScriptCharsPre", cpre)
        add("kScriptCharsPost", cpost)
        add("kScriptPrevPre", ppre)
        add("kScriptPrevPost", ppost)
        add("kScriptTailHead", tail_head)
        add("kScriptTailEnd", segs[4])
        notes.append("// 正片：Seg0 + 时长 + Seg1 + 画风 + Seg2 + 字数 + Rules")
        notes.append("//       + [CharsPre + 角色名 + CharsPost]")
        notes.append("//       + [PrevPre + 前情 + PrevPost]")
        notes.append("//       + TailHead + 梗概 + TailEnd")

        # ---------------- 选题 ----------------
        def premise_of(style, keywords="", existing=None):
            return sc.build_premise_prompt(keywords, style, count=COUNT,
                                           existing=existing)

        pbase = premise_of(StyleLine.REALISTIC)
        psegs, pk, phr, pha = _hint_split(pbase, premise_of(StyleLine.ANIME),
                                          [COUNT])
        assert pk == 0, "选题的画风提示应该在数量之前"
        add("kPremiseSeg0", psegs[0])
        add("kPremiseHintRealistic", phr)
        add("kPremiseHintAnime", pha)
        add("kPremiseSeg1", psegs[1])

        kpre, kpost = _optional_block(
            pbase, premise_of(StyleLine.REALISTIC, keywords=KEYWORDS), KEYWORDS)
        epre, epost = _optional_block(
            pbase, premise_of(StyleLine.REALISTIC, existing=[EXISTING]),
            EXISTING)

        we = split_by(premise_of(StyleLine.REALISTIC, existing=[EXISTING]),
                      [COUNT, EXISTING])
        assert we[2].startswith(epost)
        ptail = we[2][len(epost):]
        prules = psegs[2][:len(psegs[2]) - len(ptail)]
        assert psegs[2] == prules + ptail

        add("kPremiseRules", prules)
        add("kPremiseKeywordsPre", kpre)
        add("kPremiseKeywordsPost", kpost)
        add("kPremiseExistingPre", epre)
        add("kPremiseExistingPost", epost)
        add("kPremiseTailEnd", ptail)
        notes.append("// 选题：Seg0 + 画风 + Seg1 + 数量 + Rules")
        notes.append("//       + [KeywordsPre + 关键词 + KeywordsPost]")
        notes.append("//       + [ExistingPre + 已有方向 + ExistingPost]")
        notes.append("//       + TailEnd")

        # ---------------- 预告片 ----------------
        def trailer_of(style, episodes="", characters=None):
            return sc.build_trailer_prompt(PREMISE, GFloat(DUR), style,
                                           episodes=episodes,
                                           characters=characters)

        tbase = trailer_of(StyleLine.REALISTIC)
        tsegs, tk, thr, tha = _hint_split(tbase, trailer_of(StyleLine.ANIME),
                                          [DUR, CHARS, PREMISE])
        assert tk == 0, "预告片的画风提示应该在时长之前"
        add("kTrailerSeg0", tsegs[0])
        add("kTrailerHintRealistic", thr)
        add("kTrailerHintAnime", tha)
        add("kTrailerSeg1", tsegs[1])
        add("kTrailerSeg2", tsegs[2])

        tcpre, tcpost = _optional_block(
            tbase, trailer_of(StyleLine.REALISTIC, characters=[NAMES]), NAMES)
        tepre, tepost = _optional_block(
            tbase, trailer_of(StyleLine.REALISTIC, episodes=EPISODES), EPISODES)

        wt = split_by(trailer_of(StyleLine.REALISTIC, episodes=EPISODES),
                      [DUR, CHARS, EPISODES, PREMISE])
        assert wt[3].startswith(tepost)
        ttail_head = wt[3][len(tepost):]
        trules = tsegs[3][:len(tsegs[3]) - len(ttail_head)]
        assert tsegs[3] == trules + ttail_head

        add("kTrailerRules", trules)
        add("kTrailerCharsPre", tcpre)
        add("kTrailerCharsPost", tcpost)
        add("kTrailerEpisodesPre", tepre)
        add("kTrailerEpisodesPost", tepost)
        add("kTrailerTailHead", ttail_head)
        add("kTrailerTailEnd", tsegs[4])
        notes.append("// 预告片：Seg0 + 画风 + Seg1 + 时长 + Seg2 + 字数 + Rules")
        notes.append("//         + [CharsPre + 角色名 + CharsPost]")
        notes.append("//         + [EpisodesPre + 已写剧集 + EpisodesPost]")
        notes.append("//         + TailHead + 梗概 + TailEnd")

        # ---------------- 输出 ----------------
        out = io.StringIO()
        out.write(header([
            "剧本、选题、预告片三个提示词。都是行 join 出来的，带条件块，",
            "所以分两层切：Seg/Rules/Tail 是必然出现的，Block 是可选的。",
            "拼接顺序见下面的注释。",
        ]))
        for line in notes:
            out.write(line + "\n")
        out.write("\n")
        for name, val in emit:
            out.write("inline constexpr const char* %s =\n    %s;\n\n"
                      % (name, lit(val)))

        out.write("// 说话人为空的各种写法。模型经常无视 schema 填 none、旁白 这类词，\n")
        out.write("// 原样当名字用的话，成片字幕上会出现「none：寂静」。\n")
        out.write("inline constexpr const char* kNoSpeaker[] = {\n")
        for w in sorted(sc._NO_SPEAKER):
            out.write("    " + lit(w) + ",\n")
        out.write("};\n\n")

        out.write("// 整段外面套的括号和引号。左右相同的（引号）判断规则不一样，\n")
        out.write("// 见 C++ 侧 wraps_whole 的注释。\n")
        out.write("inline constexpr const char* kWrappers[][2] = {\n")
        for l, r in sc._WRAPPERS:
            out.write("    {" + lit(l) + ", " + lit(r) + "},\n")
        out.write("};\n\n")

        out.write("// 只削开头这几种括号里的时间码。\n")
        out.write("inline constexpr const char* kLeadBrackets[][2] = {\n")
        for l, r in sc._LEAD_BRACKETS:
            out.write("    {" + lit(l) + ", " + lit(r) + "},\n")
        out.write("};\n\n")

        out.write("// 括号里出现这些才算时间码。光有数字不够——\n")
        out.write("// 「（他犹豫了3秒）」和「（第3次）」得区分开。\n")
        out.write("inline constexpr const char* kTimeUnits[] = {\n")
        for u in sc._TIME_UNIT:
            out.write("    " + lit(u) + ",\n")
        out.write("};\n\n")

        out.write("// 一集里对白占的比重。剩下的是动作和环境描写，\n")
        out.write("// 不发声但占画面时长。全是对白的话成片像念稿子。\n")
        out.write("inline constexpr double kDialogueShare = %r;\n"
                  % sc.DIALOGUE_SHARE)
        from changji.stages.audio import CHARS_PER_SECOND
        out.write("inline constexpr double kCharsPerSecond = %r;\n\n"
                  % CHARS_PER_SECOND)

        out.write("// 塞进提示词前要截断的长度，按**字符**不是字节。\n")
        out.write("inline constexpr std::size_t kPrevMaxChars = 4000;\n")
        out.write("inline constexpr std::size_t kEpisodesMaxChars = 6000;\n")
        out.write("inline constexpr std::size_t kExistingMaxChars = 60;\n")
        out.write("inline constexpr std::size_t kExistingMaxItems = 6;\n\n")

        out.write("}  // namespace changji::stages::prompt\n")
    finally:
        sc.budget_chars = real_budget

    path = "cpp/src/stages/script_prompt.inc.hpp"
    io.open(path, "w", encoding="utf-8", newline="\n").write(out.getvalue())
    print("写好了", path, "（%d 段）" % len(emit))


# ---- 大模型平台清单 ----

def gen_providers() -> None:
    """把 LLM_PROVIDERS 原样嵌进 C++。

    十几家平台，每家四个字段，手抄一遍必然错。而且这份清单会随各家的
    地址变化而更新——手抄的那份只会越来越旧，且没有任何迹象提示它旧了。
    """
    from changji.web.server import LLM_PROVIDERS

    dumped = json.dumps(LLM_PROVIDERS, ensure_ascii=False, indent=1)
    out = io.StringIO()
    out.write(header([
        "常见大模型平台的接入地址，从 web/server.py 的 LLM_PROVIDERS 原样导出。",
        "各家地址会变，改了 Python 侧就重跑这个脚本。",
    ]))
    out.write("inline constexpr const char* kLlmProvidersJson =\n    "
              + lit(dumped) + ";\n\n")
    out.write("}  // namespace changji::stages::prompt\n")

    path = "cpp/src/http/llm_providers.inc.hpp"
    io.open(path, "w", encoding="utf-8", newline="\n").write(out.getvalue())
    print("写好了", path, "（%d 家）" % len(LLM_PROVIDERS))


if __name__ == "__main__":
    gen_bible()
    gen_storyboard()
    gen_shot_schema()
    gen_script()
    gen_providers()
