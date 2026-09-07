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


if __name__ == "__main__":
    gen_bible()
    gen_storyboard()
    gen_shot_schema()
