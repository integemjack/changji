#!/bin/sh
# 首次启动时写一份容器版配置，之后再也不碰它。
#
# 为什么不用环境变量直接注入地址：环境变量优先级最高，会永久盖住
# 用户在界面上改的值。那样「保存」按钮就是假的，重启一次就退回去。
# 所以只在配置文件不存在时播一次种，之后界面说了算。
set -e

CFG_DIR="${CHANGJI_CONFIG_DIR:-/root/.config/changji}"
CFG="$CFG_DIR/config.toml"

if [ ! -f "$CFG" ]; then
  mkdir -p "$CFG_DIR"
  cat > "$CFG" <<TOML
# 场记配置文件（容器首次启动时生成）
# 界面上「环境」页可以直接改这些，改完会写回本文件。
# 优先级：环境变量 > 项目目录下的 changji.toml > 本文件 > 内置默认值

vram_gb_override = ${CHANGJI_SEED_VRAM_GB:-16}

[llm]
# **播的是 remote，不是内置的那条。** 引擎默认 backend = "local"（权重
# 在 [models].llm），而镜像里没有权重；容器场景下大模型本来就该是
# compose 里那个 ollama 或者别的机器。要用进程内的，在界面上改回 local。
backend = "${CHANGJI_SEED_LLM_BACKEND:-remote}"
# 同一个 compose 里的服务用服务名互访，不是 localhost。
base_url = "${CHANGJI_SEED_LLM_URL:-http://ollama:11434/v1}"
model = "${CHANGJI_SEED_LLM_MODEL:-qwen3:14b}"

[tts]
# local 是进程内配音，要 [models].tts / tts_decoder 那两个权重；
# 没下权重的话在界面上改成 http、填一个外部配音服务的地址。
backend = "${CHANGJI_SEED_TTS_BACKEND:-local}"
engine = "cosyvoice3"

[gates]
enabled = true
max_attempts_per_shot = 3
fallback_on_exhausted = true

[assembly]
fps = 24
crf = 18
scene_transition_s = 0.4
subtitle_max_chars_per_line = 15
subtitle_font = "Source Han Sans SC"
TOML
  echo "已生成配置 $CFG"
fi

exec "$@"
