r"""把所有金语料重新生成一遍，看还对不对得上。

跑法（在 changji/ 目录下）：
    .venv/Scripts/python.exe cpp/tools/regen_golden.py          # 只查，不改
    .venv/Scripts/python.exe cpp/tools/regen_golden.py --write  # 把变化留下

**为什么需要这个。** `tests/golden/` 那几十份 JSON 是删掉 Python 之后
唯一活下来的安全网——单元测试直接读文件，不碰 Python。可它们是**一次性
导出来就进版本库**的，此后再没人问过：

  - 那个生成器现在还跑得起来吗？（Python 侧改了名字、改了签名，
    生成器就废了，而语料还在，测试还绿）
  - 它现在导出来的，和版本库里那份还一样吗？（Python 侧行为变了，
    语料却停在旧答案上——**测试钉的是历史，不是现状**）

这两种情况都不会有任何东西报错。测试照样全绿，因为它读的是文件。
阶段 8 的闸门是"对拍覆盖到你愿意永久放弃它的程度"，
而一份**生不出来**或**已经和 Python 对不上**的语料，覆盖的是零。

所以这个脚本干两件事：把每个生成器跑一遍（跑不起来就是废了），
再看产出和版本库里的差别（有差别就是漂了）。

**有几样东西每次跑都不一样，得先抹平再比。** 第一次跑这个脚本，
四份语料"对不上"，看下去全是这两类：

  - `endpoints_*.json` 里存着**导出那次的临时目录绝对路径**，
    像 `C:\Users\...\Temp\changji_episodes_8hbmemsa\case01`——
    随机后缀每次都变
  - `project.json` 里的 `created_at` / `updated_at`

都不是漂移。可要是不抹平，这个脚本**每次都报四份不一致**，
于是它就变成了一个没人看的脚本——而真漂移正好躲在那四行噪音里。
所以抹平规则写在下面 `NOISE` 里，**只抹这两类，抹掉什么会打印出来**，
剩下任何差别都是真的。

（顺带记一笔：语料里存着我这台机器的绝对路径，换台机器重新生成必然
全不一样。测试读的是提交进版本库的那份文件，所以不影响测试；
影响的是"能不能在别的机器上验这份语料还对不对"。)

⚠️ 默认**只查不改**：跑完把 `tests/golden/` 恢复回版本库里的样子。
所以跑之前那个目录必须是干净的——不然会把没提交的改动一起冲掉。
脏的话脚本直接拒绝跑，不会替你决定哪些该留。
"""
from __future__ import annotations

import argparse
import io
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve()
CPP = HERE.parents[1]
REPO = CPP.parent                 # changji/ —— 生成器都从这里跑
GOLDEN = CPP / "tests" / "golden"

# **git 报的路径是相对仓库根的，不是相对 REPO。** 这两个不是一回事：
# 仓库根在上一层（changji/changji 是项目目录）。拿 REPO 去拼 git 给的
# 相对路径，会拼出一个不存在的双层路径，然后每个文件都落进"读不到"
# 那一支，被当成漂移——第一版就是这么把四份噪音报成了四份真漂移。
GIT_ROOT = Path(subprocess.run(
    ["git", "rev-parse", "--show-toplevel"], cwd=REPO,
    capture_output=True, text=True, encoding="utf-8",
).stdout.strip() or REPO)

# 不产金语料的，跑了也没意义
SKIP = {
    "gen_eaw.py",        # 东亚字宽表，数据来自 Unicode，不碰 Python 引擎
    "gen_workflows.py",  # 把工作流 JSON 嵌进源码
    "gen_prompts.py",    # 提示词模板嵌入
    "fake_llm.py",
    "serve_python.py",
    "audit_routes.py",
    "contract_audit.py",
    "coverage_audit.py",
    "check_template_compat.py",
    "regen_golden.py",
}

# 每次跑都不一样、又确实不算漂移的东西。**只有这两类**，
# 多抹一条就多一分把真漂移抹掉的风险，所以每条都写清抹的是什么。
NOISE = [
    # tempfile 的随机后缀：changji_episodes_8hbmemsa -> changji_episodes_<临时>
    (re.compile(r'(changji_[A-Za-z\u4e00-\u9fff]*_)[0-9a-z_]{8}'),
     r'\1<临时>'),
    # ISO 时间戳：2026-09-08T14:43:04.773424+00:00
    (re.compile(r'\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d+)?'
                r'([+-]\d{2}:\d{2}|Z)?'),
     '<时间>'),
]


def denoise(text: str) -> tuple[str, int]:
    """抹平噪音，并数一共抹了几处。"""
    n = 0
    for pat, rep in NOISE:
        text, k = pat.subn(rep, text)
        n += k
    return text, n


def git(*args: str) -> str:
    r = subprocess.run(["git", *args], cwd=REPO, capture_output=True,
                       text=True, encoding="utf-8", errors="replace")
    return (r.stdout or "").strip()


def git_raw(*args: str) -> str:
    """不 strip。比文件内容时末尾那个换行也算数。"""
    r = subprocess.run(["git", *args], cwd=REPO, capture_output=True,
                       text=True, encoding="utf-8", errors="replace")
    return r.stdout or ""


def golden_dirty() -> list[str]:
    """回相对仓库根的路径。

    用 `-z`：porcelain 默认会把非 ASCII 路径转义成 `"\351\241\271..."`，
    这个项目的语料目录名是中文的，反转义一步走错就成乱码。
    `-z` 直接给原样路径，NUL 分隔，没有引号也没有转义。
    """
    out = git_raw("status", "--porcelain", "-z", "--", str(GOLDEN))
    rels = []
    for rec in out.split("\0"):
        if len(rec) > 3:
            rels.append(rec[3:])
    return rels


def generators() -> list[Path]:
    out = []
    out += sorted((CPP / "tests").glob("export_*.py"))
    for p in sorted((CPP / "tools").glob("gen_*.py")):
        out.append(p)
    return [p for p in out if p.name not in SKIP]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true",
                    help="把重新生成的结果留下（默认跑完就恢复）")
    args = ap.parse_args()

    dirty = golden_dirty()
    if dirty and not args.write:
        print("拒绝跑：tests/golden/ 里有没提交的改动。")
        print("默认模式跑完要把这个目录恢复回版本库的样子，"
              "现在跑会把下面这些一起冲掉：")
        for l in dirty:
            print("  " + l)
        print("\n先提交或 stash，或者加 --write 明确表示要留下新结果。")
        return 2

    gens = generators()
    print(f"{len(gens)} 个生成器，{len(list(GOLDEN.glob('*.json')))} 份语料\n")

    broken: list[tuple[str, str]] = []
    ok = 0
    for g in gens:
        t0 = time.monotonic()
        r = subprocess.run([sys.executable, str(g)], cwd=REPO,
                           capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=900)
        dt = time.monotonic() - t0
        if r.returncode != 0:
            tail = ((r.stderr or "") + (r.stdout or "")).strip().splitlines()
            broken.append((g.name, tail[-1] if tail else f"退出码 {r.returncode}"))
            print(f"  ✗ {g.name:36} 跑不起来")
        else:
            ok += 1
            print(f"  · {g.name:36} {dt:5.1f}s")

    print()
    changed = [l for l in golden_dirty()]

    print("=" * 74)
    if broken:
        print(f"**{len(broken)} 个生成器跑不起来**——它们导的语料等于没有源头了：")
        for name, why in broken:
            print(f"  {name}: {why}")
        print()

    drifted: list[str] = []
    noisy: list[tuple[str, int]] = []
    for rel in changed:
        f = GIT_ROOT / rel
        was = git_raw("show", f"HEAD:{rel}")
        if not was:
            # 版本库里根本没有这个文件 = 新语料，不是漂移
            print(f"  （新语料，版本库里还没有）{rel}")
            continue
        try:
            now = io.open(f, encoding="utf-8").read()
        except OSError:
            drifted.append(rel)
            continue
        a, _ = denoise(was)
        b, k = denoise(now)
        if a == b:
            noisy.append((rel, k))
        else:
            drifted.append(rel)

    if noisy:
        print(f"{len(noisy)} 份只差在每次跑都不一样的东西上（已按规则抹平）：")
        for rel, k in noisy:
            print(f"  {rel}  抹了 {k} 处")
        print()

    if drifted:
        print(f"**{len(drifted)} 份是真的对不上**"
              f"——要么 Python 侧变了，要么语料没跟上：")
        for rel in drifted:
            print("  " + rel)
        print()
        print("逐份看差别：git diff -- <文件>")
    else:
        print("抹平之后，所有语料重新生成的结果和版本库里一致。")

    if not args.write:
        # 恢复。上面已经确认过跑之前是干净的，所以这里冲掉的只有本次产出。
        git("checkout", "--", str(GOLDEN))
        print("\n（已恢复 tests/golden/。要留下新结果加 --write）")

    print(f"\n生成器 {ok}/{len(gens)} 跑得起来，"
          f"真漂移 {len(drifted)} 份，只是噪音 {len(noisy)} 份")
    return 1 if (broken or drifted) else 0


if __name__ == "__main__":
    raise SystemExit(main())
