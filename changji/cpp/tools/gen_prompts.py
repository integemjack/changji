"""从 Python 的提示词函数里抽出模板，生成 C++ 的原始字符串字面量。

跑法（在 changji/ 目录下）：
    .venv/Scripts/python.exe cpp/tools/gen_prompts.py

产出 cpp/src/stages/*_prompt.inc.hpp。改了 Python 侧的提示词之后要重跑，
然后重新生成对拍语料（cpp/tests/export_bible_golden.py）。

手抄这段文字是不可接受的：几百个汉字里错一个，提示词就和 Python 不一致了，
而这类差异不会报错，只会让模型的输出悄悄变一点，要跑几十个镜头才看得出来。
"""
import io
import sys

sys.path.insert(0, "src")  # 从 changji/ 跑
from changji.models.character import StyleLine
from changji.stages.bible import build_prompt

MARK = "\x00SCRIPT\x00"

real = build_prompt(MARK, StyleLine.REALISTIC)
anime = build_prompt(MARK, StyleLine.ANIME)

# 两者只有 style_hint 那一句不同。找出公共前后缀。
i = 0
while real[i] == anime[i]:
    i += 1
j = 0
while real[-1 - j] == anime[-1 - j]:
    j += 1

prefix = real[:i]
real_hint = real[i:len(real) - j]
anime_hint = anime[i:len(anime) - j]
suffix = real[len(real) - j:]

assert MARK in suffix, "剧本占位符应该落在公共后缀里"
mid, tail = suffix.split(MARK)

# 原始字符串字面量的分隔符要保证不出现在内容里
DELIM = "CJ"
for part in (prefix, real_hint, anime_hint, mid, tail):
    assert ')' + DELIM + '"' not in part, "分隔符撞车了，换一个"


def lit(s):
    return 'R"' + DELIM + '(' + s + ')' + DELIM + '"'


out = io.StringIO()
out.write("// 本文件由 cpp/tools/gen_prompts.py 从 Python 的 build_prompt\n")
out.write("// 生成，不要手改。提示词要求和 Python 逐字节一致，手抄错一个字\n")
out.write("// 不会报错，只会让模型输出悄悄变一点。\n\n")
out.write("#pragma once\n\nnamespace changji::stages::prompt {\n\n")
out.write("inline constexpr const char* kBiblePrefix =\n    " + lit(prefix) + ";\n\n")
out.write("inline constexpr const char* kBibleHintRealistic =\n    " + lit(real_hint) + ";\n\n")
out.write("inline constexpr const char* kBibleHintAnime =\n    " + lit(anime_hint) + ";\n\n")
out.write("inline constexpr const char* kBibleMiddle =\n    " + lit(mid) + ";\n\n")
out.write("inline constexpr const char* kBibleTail =\n    " + lit(tail) + ";\n\n")
out.write("}  // namespace changji::stages::prompt\n")

target = "cpp/src/stages/bible_prompt.inc.hpp"
io.open(target, "w", encoding="utf-8", newline="\n").write(out.getvalue())
print("写好了", target)
print("前缀", len(prefix), "写实提示", len(real_hint), "动漫提示", len(anime_hint))
print("中段", len(mid), "尾段", len(tail))
