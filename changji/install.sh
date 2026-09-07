#!/usr/bin/env bash
# 场记一键安装（不使用 Docker）。
#
# 用法：
#   curl -fsSL https://raw.githubusercontent.com/<repo>/main/install.sh | bash
# 或者在仓库目录里：
#   bash install.sh
#
# 装完之后：
#   changji doctor     体检
#   changji serve      启动 Web 界面
#
# 这个脚本只装编排引擎和它的系统依赖。ComfyUI 和大模型服务是独立的，
# 它们可以在本机也可以在别的机器上，装好后用 changji doctor 指路。

set -euo pipefail

PREFIX="${CHANGJI_PREFIX:-$HOME/.changji}"
PYTHON_MIN="3.11"

info()  { printf '\033[36m==>\033[0m %s\n' "$*"; }
warn()  { printf '\033[33m警告:\033[0m %s\n' "$*" >&2; }
die()   { printf '\033[31m失败:\033[0m %s\n' "$*" >&2; exit 1; }

# ---- 系统识别 ----

detect_pm() {
  for pm in apt-get dnf yum pacman zypper apk brew; do
    if command -v "$pm" >/dev/null 2>&1; then echo "$pm"; return; fi
  done
  echo ""
}

need_sudo() {
  if [ "$(id -u)" -eq 0 ]; then echo ""; else echo "sudo"; fi
}

install_pkgs() {
  local pm; pm="$(detect_pm)"
  local sudo_cmd; sudo_cmd="$(need_sudo)"
  [ -z "$pm" ] && return 1
  case "$pm" in
    apt-get) $sudo_cmd apt-get update -qq && $sudo_cmd apt-get install -y -qq "$@" ;;
    dnf|yum) $sudo_cmd "$pm" install -y -q "$@" ;;
    pacman)  $sudo_cmd pacman -Sy --noconfirm --quiet "$@" ;;
    zypper)  $sudo_cmd zypper --non-interactive --quiet install "$@" ;;
    apk)     $sudo_cmd apk add --quiet "$@" ;;
    brew)    brew install "$@" ;;
  esac
}

# ---- 依赖检查 ----

check_python() {
  local py=""
  for cand in python3.13 python3.12 python3.11 python3 python; do
    if command -v "$cand" >/dev/null 2>&1; then
      if "$cand" -c "import sys; raise SystemExit(0 if sys.version_info>=(3,11) else 1)" 2>/dev/null; then
        py="$cand"; break
      fi
    fi
  done
  if [ -z "$py" ]; then
    info "没有找到 Python $PYTHON_MIN 或更高版本，尝试安装"
    install_pkgs python3 python3-venv python3-pip \
      || die "自动安装 Python 失败。请手动装 Python $PYTHON_MIN 以上再重试"
    py="python3"
  fi
  echo "$py"
}

check_ffmpeg() {
  if command -v ffmpeg >/dev/null 2>&1 && command -v ffprobe >/dev/null 2>&1; then
    return 0
  fi
  info "没有找到 ffmpeg，尝试安装（装配环节的硬依赖）"
  install_pkgs ffmpeg || {
    warn "自动安装 ffmpeg 失败。装配环节会用不了。"
    warn "请手动安装后重跑 changji doctor。"
    return 1
  }
}

check_fonts() {
  # 中文字幕烧录需要中文字体，缺了会渲染成方框
  if fc-list 2>/dev/null | grep -qiE "noto sans cjk|source han sans|wqy|pingfang"; then
    return 0
  fi
  info "没有找到中文字体，尝试安装（字幕烧录需要）"
  install_pkgs fonts-noto-cjk 2>/dev/null \
    || install_pkgs google-noto-sans-cjk-fonts 2>/dev/null \
    || install_pkgs noto-fonts-cjk 2>/dev/null \
    || warn "自动安装中文字体失败。中文字幕可能显示为方框。"
}

# ---- 安装 ----

main() {
  info "安装位置 $PREFIX"

  local py; py="$(check_python)"
  info "使用 $($py --version 2>&1)"

  check_ffmpeg || true
  check_fonts || true

  info "创建虚拟环境"
  "$py" -m venv "$PREFIX/venv" || die "创建虚拟环境失败"
  local pip="$PREFIX/venv/bin/pip"
  "$pip" install --quiet --upgrade pip

  info "安装场记"
  if [ -f "$(dirname "$0")/pyproject.toml" ]; then
    "$pip" install --quiet "$(dirname "$0")"     # 从本地仓库装
  else
    "$pip" install --quiet changji                # 从包索引装
  fi

  # 放一个可执行文件到 PATH 上常见的位置
  local bindir="$HOME/.local/bin"
  mkdir -p "$bindir"
  ln -sf "$PREFIX/venv/bin/changji" "$bindir/changji"

  info "安装完成"
  echo
  if ! echo ":$PATH:" | grep -q ":$bindir:"; then
    warn "$bindir 不在 PATH 里。把下面这行加进你的 shell 配置："
    echo "    export PATH=\"\$HOME/.local/bin:\$PATH\""
    echo
  fi
  echo "下一步："
  echo "    changji doctor     检查环境和外部服务"
  echo "    changji serve      启动 Web 界面"
  echo
  echo "推理服务 ComfyUI 和大模型服务需要单独部署，可以在本机也可以在别的机器上。"
  echo "部署好之后用环境变量或配置文件指路："
  echo "    export CHANGJI_COMFY_BASE_URL=http://某台机器:8188"
  echo "    export CHANGJI_LLM_BASE_URL=http://某台机器:11434/v1"
}

main "$@"
