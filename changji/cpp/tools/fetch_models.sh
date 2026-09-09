#!/bin/bash
# 在 Linux 服务器上下模型。
#
# 跑法：
#     bash cpp/tools/fetch_models.sh /data/models [并发数]
#
# Windows 那份是 download_models.ps1，这份是它的 Linux 对应物。
# 两份的文件清单要一致——**改了一份记得改另一份**。
#
# 下的是「全尺寸档」，见方案「8 × L20（48 GB）这台机器：算出来的账」：
#
#   扩散模型走 bf16，文本编码器**下两份**：
#     fp8_scaled  8.74 GB —— 和 bf16 扩散模型加起来 47 GB，一张 48 GB 卡装得下，
#                            八路并行直接成立，不用 offload
#     bf16       15.45 GB —— 加起来 53.5 GB > 48 GB，要靠 offload
#                            （机器内存 1 TB，8 个进程 × 53.5 GB 装得下）
#   两份都留着，配置里换一行 image_text_encoder 就切，用来对比画质。
#
# **写剧本那个 Qwen3-14B 没在里面**：流水线到今天一次都没用过它
# （剧本分镜走外部 Ollama 或手写分镜表）。要进程内跑再单独加。

set -u

DEST="${1:-/data/models}"
JOBS="${2:-3}"

# 名字|大小MB|URL
FILES=$(cat <<'EOF'
qwen_image_edit_bf16.safetensors|38963|https://huggingface.co/Comfy-Org/Qwen-Image-Edit_ComfyUI/resolve/main/split_files/diffusion_models/qwen_image_edit_bf16.safetensors
qwen_2.5_vl_7b_fp8_scaled.safetensors|8950|https://huggingface.co/Comfy-Org/Qwen-Image_ComfyUI/resolve/main/split_files/text_encoders/qwen_2.5_vl_7b_fp8_scaled.safetensors
qwen_2.5_vl_7b_bf16.safetensors|15821|https://huggingface.co/Comfy-Org/Qwen-Image_ComfyUI/resolve/main/split_files/text_encoders/qwen_2.5_vl_7b.safetensors
qwen_image_vae.safetensors|246|https://huggingface.co/Comfy-Org/Qwen-Image_ComfyUI/resolve/main/split_files/vae/qwen_image_vae.safetensors
wan2.2_ti2v_5B_fp16.safetensors|9534|https://huggingface.co/Comfy-Org/Wan_2.2_ComfyUI_Repackaged/resolve/main/split_files/diffusion_models/wan2.2_ti2v_5B_fp16.safetensors
Wan2.2_VAE.safetensors|1344|https://huggingface.co/Comfy-Org/Wan_2.2_ComfyUI_Repackaged/resolve/main/split_files/vae/wan2.2_vae.safetensors
umt5_xxl_fp16.safetensors|10844|https://huggingface.co/Comfy-Org/Wan_2.2_ComfyUI_Repackaged/resolve/main/split_files/text_encoders/umt5_xxl_fp16.safetensors
Qwen3-TTS-12Hz-1.7B-Base-bf16.gguf|3308|https://huggingface.co/ggml-org/Qwen3-TTS-12Hz-1.7B-Base-GGUF/resolve/main/Qwen3-TTS-12Hz-1.7B-Base-bf16.gguf
mmproj-Qwen3-TTS-12Hz-1.7B-Base-bf16.gguf|635|https://huggingface.co/ggml-org/Qwen3-TTS-12Hz-1.7B-Base-GGUF/resolve/main/mmproj-Qwen3-TTS-12Hz-1.7B-Base-bf16.gguf
EOF
)

mkdir -p "$DEST"

# ---- 先算还差多少，不是算一共多少 ----
#
# Windows 那份栽过这个：把清单里所有文件加起来跟剩余空间比，
# 于是目录一超过盘的大小就永远不让下，哪怕一个字节都不缺。
need=0
total=0
while IFS='|' read -r name mb url; do
    [ -z "$name" ] && continue
    total=$((total + mb))
    have=0
    [ -f "$DEST/$name" ] && have=$(( $(stat -c%s "$DEST/$name") / 1048576 ))
    left=$((mb - have))
    [ "$left" -gt 0 ] && need=$((need + left))
done <<< "$FILES"

free_mb=$(df -Pm "$DEST" | awk 'NR==2 {print $4}')
echo "落盘目录：$DEST"
echo "清单合计 $((total / 1024)) GB，还差 $((need / 1024)) GB，盘上剩 $((free_mb / 1024)) GB"
if [ "$need" -gt $((free_mb - 20480)) ]; then
    echo "空间不够（要留 20 GB 余量）" >&2
    exit 1
fi
echo

# ---- 下一个文件。**整个文件重试，按有没有进展判停** ----
#
# 几十 GB 的文件在长连接上断是常事。curl 自己的 --retry 管的是"请求失败"，
# 对传到一半断线不一定重来；-C - 能续传，所以从外面再叫一次就是接着下。
# 判停的条件是"连着几次一个字节都没多"——那才是真的卡住了。
fetch_one() {
    local name="$1" mb="$2" url="$3"
    local out="$DEST/$name"
    local stall=0 try=0

    while [ "$stall" -lt 5 ]; do
        try=$((try + 1))
        local before=0
        [ -f "$out" ] && before=$(stat -c%s "$out")

        # 已经完整了就不动它（留 1% 余量，HF 报的大小和实际偶有出入）
        if [ "$before" -ge $((mb * 1048576 * 99 / 100)) ]; then
            echo "  跳过（已有）：$name"
            return 0
        fi

        curl -sS -L --fail --retry 3 --retry-delay 5 --retry-all-errors \
             -C - -o "$out" "$url" && { echo "  好了：$name"; return 0; }

        local after=0
        [ -f "$out" ] && after=$(stat -c%s "$out")
        if [ "$after" -gt "$before" ]; then
            stall=0
            echo "  续传中：$name（$((after / 1048576))/$mb MB，第 $try 次）"
        else
            stall=$((stall + 1))
        fi
        sleep 3
    done
    echo "  失败：$name（连着 5 次没进展）" >&2
    return 1
}

export -f fetch_one
export DEST

echo "开始下，并发 $JOBS ——"
# xargs 做并发。**不要用 & 自己攒**：几十 GB 的文件全并发会把带宽切碎，
# 而且断线重试的时候一起重试。
echo "$FILES" | grep -v '^$' | \
    xargs -P "$JOBS" -I{} bash -c '
        IFS="|" read -r n m u <<< "{}"
        fetch_one "$n" "$m" "$u"
    '

echo
echo "==================== 下完了 ===================="
ls -la "$DEST" | awk 'NR>1 {s+=$5; printf "  %8.2f GB  %s\n", $5/1073741824, $9} END {printf "  ---- 合计 %.1f GB\n", s/1073741824}'
cat <<EOF

配置里这么填（changji.toml 的 [models]）：

[models]
engine = "sd"
dir = "$DEST"
image = "qwen_image_edit_bf16.safetensors"
image_vae = "qwen_image_vae.safetensors"
# 装得下的那一档，八路并行不用 offload
image_text_encoder = "qwen_2.5_vl_7b_fp8_scaled.safetensors"
# 想对比画质就换成这个（要 offload，机器内存够）
# image_text_encoder = "qwen_2.5_vl_7b_bf16.safetensors"
video = "wan2.2_ti2v_5B_fp16.safetensors"
video_vae = "Wan2.2_VAE.safetensors"
video_text_encoder = "umt5_xxl_fp16.safetensors"
tts = "Qwen3-TTS-12Hz-1.7B-Base-bf16.gguf"
tts_decoder = "mmproj-Qwen3-TTS-12Hz-1.7B-Base-bf16.gguf"
EOF
