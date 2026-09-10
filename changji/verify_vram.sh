#!/bin/bash
# 显存调度这一轮改动的真机验收。**必须在有卡的机器上跑。**
#
#     bash verify_vram.sh [构建目录=/root/autodl-tmp/build-all] [端口=8123]
#
# 为什么要有这个脚本：2026-09-10 这一轮为了做到"显存够就不清理大模型"，
# 加了一层显存估算（SlotSpec::live_vram + ModelsConfig::*_live_vram_gb）。
# 单元测试能盯住口径和时机——数从哪来、什么时候问、问不到怎么退——
# **但盯不住那个数对不对**。"20 GB 的模型加上计算缓冲在 32 GB 卡上装不装
# 得下"只有真卡答得了，而估低了的代价是 CUDA OOM 整轮出片作废。
#
# 所以这里查的全是"算出来的 = 量出来的"，一条都不靠单元测试。
set -u
# 只量一眼显存就走，给下面那段人工步骤反复用。
if [ "${1:-}" = "--mem" ]; then
    nvidia-smi --query-gpu=memory.used,memory.total --format=csv,noheader
    exit 0
fi
BUILD="${1:-/root/autodl-tmp/build-all}"
PORT="${2:-8123}"
BIN="$BUILD/changji"
# Windows 上编出来带 .exe。带上这一支纯粹是为了**这个脚本本身能被验**——
# 服务器一直连不上的时候，至少能在本机把前三段跑通，确认脚本不是空转的。
[ -x "$BIN" ] || [ ! -x "$BIN.exe" ] || BIN="$BIN.exe"
FAIL=0

say()  { echo "[$(date '+%H:%M:%S')] $*"; }
ok()   { echo "  ✓ $*"; }
bad()  { echo "  ✗ $*"; FAIL=$((FAIL+1)); }
# **查不了 ≠ 没过。** 缺个工具就记一笔失败的话，最后那个数就没意义了，
# 而且会盖住真正没过的那几项。
skip() { echo "  － 跳过：$*"; }

used_mib() {
    nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits 2>/dev/null | head -1
}

# ---- 0. 先确认真有卡 ----
#
# 无卡模式下 nvidia-smi **退出码 0 但一个字都不打**，不是报错。
# 照着往下跑的话每一步都"通过"，而实际什么都没验到——比不跑更糟。
say "确认显卡"
if ! nvidia-smi -L 2>/dev/null | grep -q GPU; then
    echo "  ✗ 没有显卡（无卡模式？nvidia-smi -L 是空的）。"
    echo "    这个脚本查的全是真机显存行为，无卡跑等于没跑。"
    echo "    去 AutoDL 控制台改成有卡开机再来。"
    exit 2
fi
nvidia-smi --query-gpu=name,memory.total --format=csv,noheader | sed 's/^/  /'

[ -x "$BIN" ] || { echo "  ✗ 找不到 $BIN，先编。"; exit 2; }

# ---- 1. 单元测试 ----
say "单元测试"
if [ -x "$BUILD/changji_tests" ]; then
    if "$BUILD/changji_tests" >/tmp/vram_tests.log 2>&1; then
        ok "$(grep -o '[0-9]* passed' /tmp/vram_tests.log | tail -1)"
    else
        bad "有用例挂了，看 /tmp/vram_tests.log"
    fi
else
    bad "没编 changji_tests"
fi

# ---- 2. 起进程 ----
say "起进程（一个进程跑全部）"
# 服务器上是容器、没有 systemd，用 setsid 让它脱离这个 shell。
# 退一步用 nohup：Git Bash 里没有 setsid，而能在本机跑通这个脚本
# 本身就是它靠不靠得住的一部分。
if command -v setsid >/dev/null 2>&1; then
    setsid nohup "$BIN" --port "$PORT" </dev/null >/tmp/vram_run.log 2>&1 &
else
    nohup "$BIN" --port "$PORT" </dev/null >/tmp/vram_run.log 2>&1 &
fi
for _ in $(seq 1 30); do
    curl -sf "http://127.0.0.1:$PORT/bff/settings/overview" >/dev/null 2>&1 && break
    sleep 1
done
OV=$(curl -sf "http://127.0.0.1:$PORT/bff/settings/overview" 2>/dev/null)
[ -n "$OV" ] || { bad "起不来，看 /tmp/vram_run.log"; exit 1; }
ok "起来了，端口 $PORT"
if command -v pgrep >/dev/null 2>&1; then
    N=$(pgrep -xc changji 2>/dev/null || echo 0)
    if [ "$N" = "1" ]; then ok "只有一个 changji 进程"
    else bad "有 $N 个 changji 进程，说好的一个"; fi
else
    skip "没有 pgrep，数不了进程数"
fi

# ---- 3. 程序算的放置 vs 卡上真实显存 ----
#
# 这一段是重点。placement 是程序按模型文件大小和卡的显存自己算的，
# liveVramGb 就是它认为"跑起来要占这么多"。要是这个数比整卡还大，
# 那它自己就该判成放内存却没判——**那正是 OOM 的前一步**。
say "程序算出来的权重放置"
PY_BIN=$(command -v python3 || command -v python || true)
# 找得到还不算数：Windows 上那个应用商店的占位符也会被 command -v 找到，
# 一跑却只打一句"Python was not found"。真跑一下才知道。
[ -n "$PY_BIN" ] && "$PY_BIN" -c "" >/dev/null 2>&1 || PY_BIN=""
if [ -z "$PY_BIN" ]; then
    skip "没有 python，放置这一段解不了 JSON"
else
"$PY_BIN" - "$OV" <<'PY' 
import json, subprocess, sys
ov = json.loads(sys.argv[1])
pl = ov.get("effective", {}).get("placement")
if not pl:
    print("  ✗ overview 里没有 placement，引擎是旧的？"); sys.exit(1)
total = float(subprocess.check_output(
    ["nvidia-smi","--query-gpu=memory.total","--format=csv,noheader,nounits"]
).split()[0]) / 1024.0
print(f"  卡：{total:.1f} GB（程序看到 {pl.get('cardGb',0):.1f} GB）")
if abs(total - float(pl.get("cardGb", 0))) > 1.5:
    print("  ✗ 程序看到的显存和 nvidia-smi 差太多——vram_gb_override 还顶着？")
    sys.exit(1)
bad = 0
for key, label in (("image","出首帧"), ("video","出片")):
    p = pl.get(key, {})
    live, model = float(p.get("liveVramGb",0)), float(p.get("modelGb",0))
    where = "常驻显存" if p.get("resident") else "权重放内存"
    print(f"  {label}：{p.get('weights')} / {where}，模型 {model:.1f} GB，跑起来约 {live:.1f} GB")
    if model <= 0:
        print(f"    ! 模型没配或读不到文件，这一项验不了")
        continue
    if live > total:
        print(f"    ✗ 算出来要 {live:.1f} GB 却比整卡还大——它该判成放内存却没判")
        bad += 1
    if p.get("resident") and model + 2 > total:
        print(f"    ✗ 判成常驻，但光权重就 {model:.1f} GB，装不下")
        bad += 1
sys.exit(1 if bad else 0)
PY
[ $? -eq 0 ] && ok "算出来的放置自洽" || bad "放置算错了"
fi

# ---- 4. 「显存够就不卸大模型」——真机行为 ----
#
# 单元测试里这条是拿假数跑的。真机要看的是：大模型装上之后，
# 借出图那个槽时它到底还在不在。判据用显存占用，不看日志——
# 日志能骗人，显存不能。
say "显存够就不卸大模型"
#
# **没有"预热大模型"那种接口**（查过 bff_routes.hpp，没有这一条；也不该
# 为了验收现造一个）。大模型是用到才装的——用户确认过"用的时候才加载是
# 对的"。所以这一段是半自动的：脚本量数，动作由人在界面上做。
BASE=$(used_mib); echo "  当前 ${BASE} MiB"
cat <<'STEP'
  这一段要人配合，按顺序做，每步之后回来跑一次：
      bash verify_vram.sh --mem
  1) 界面上跑一次「剧本大纲」→ 大模型装进显存，占用应明显上去
  2) 记下这时的 MiB，作为"大模型在里面"的基准
  3) 再点一次「只出首帧」，跑完之后看占用：
       没掉回第 1 步之前的水平 → 大模型留住了，"显存够就不清理"成立
       掉回去了 → 不一定是错。对照上面 placement 里出首帧那行的
                  liveVramGb：它 + 大模型 > 整卡的话，卸掉才是对的
STEP

# ---- 5. 配音那条出路够不够得着 ----
#
# 那段"改成 [tts].backend = http 接外部服务"的话，2026-09-10 之前是死代码：
# 没人注册也没人借 Slot::TTS，所以永远抛不出来。这里只查它接上了没有。
say "配音走没走调度器（顺带核一眼这个二进制是不是新的）"
# **别写死服务器路径。** 按脚本自己的位置找源码；找不到就跳过，
# 不能因为源码不在预期的地方就报"没过"。
SRC="$(cd "$(dirname "$0")" && pwd)/cpp/src/stages/tts_backends.cpp"
[ -f "$SRC" ] || SRC=/root/changji/cpp/src/stages/tts_backends.cpp
if grep -q "Slot::TTS" "$SRC" 2>/dev/null; then
    # **源码有不等于跑着的这个有。** 二进制比源码旧的话就是没重编，
    # 底下验的还是老行为——而每一项都会"通过"，那种通过最骗人。
    if [ "$BIN" -nt "$SRC" ]; then
        ok "TTS 槽注册在源码里，二进制也比源码新"
    else
        bad "源码里有 TTS 槽，但 $BIN 比源码旧——没重编，下面验的是老二进制"
    fi
elif [ -f "$SRC" ]; then
    bad "TTS 槽没注册（源码是旧的？），那句「可以外接 API」用户还是看不到"
else
    skip "找不到 tts_backends.cpp，核不了"
fi

echo
if [ "$FAIL" = "0" ]; then
    say "全过。剩下要人眼看的两件：出片时牌子上有没有「正在准备模型」，"
    say "以及设置页引擎那张卡上「权重放哪」两行对不对。"
else
    say "有 $FAIL 项没过。"
fi
exit "$FAIL"
