#!/usr/bin/env bash
# 场记一键安装（不使用 Docker）。
#
# 用法：
#   curl -fsSL https://raw.githubusercontent.com/integemjack/changji/main/changji/install.sh | bash
# 装某一版而不是最新版：
#   CHANGJI_VERSION=v1.2.0 bash install.sh
# 装每次推分支都会刷新的滚动预发布：
#   CHANGJI_VERSION=beta bash install.sh
# （默认走 /releases/latest，而预发布不在那里面，所以不写就永远是正式版）
# 已经有本地编好的二进制：
#   CHANGJI_BINARY=/path/to/changji bash install.sh
#
# 装完之后：
#   changji --doctor      体检
#   changji --port 8080   起服务，然后浏览器打开 http://127.0.0.1:8080
#
# **这个脚本装的是一个二进制。** 阶段 8 之前它建虚拟环境、pip install
# 一个 Python 包；Python 引擎删掉之后，装的东西变成 GitHub Release 上
# 那个自带前端和推理的可执行文件。ffmpeg 和中文字体仍然要装——
# 装配和字幕烧录靠它们，而那两样不适合塞进单文件。

set -euo pipefail

PREFIX="${CHANGJI_PREFIX:-$HOME/.changji}"
# 项目 2026-09-10 搬到 integemjack/changji，CI 和 release 都在那边。
# 旧仓库 Ireoo/changji 只停在 v1.1，连 beta 都没有——指着它的话这个
# 脚本会去一个没有产物的地方找包，报的是"下载失败"，而用户看不出是
# 仓库指错了。要装别处的传 CHANGJI_REPO。
REPO="${CHANGJI_REPO:-integemjack/changji}"
VERSION="${CHANGJI_VERSION:-latest}"

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

# 发布资产的命名规则，和 .github/workflows/release.yml 里那张矩阵表一致。
# 对不上的表现是 404，而 404 看不出是"这个平台没出包"还是"名字拼错了"，
# 所以这里把算出来的名字打出来。
detect_target() {
  local os arch
  case "$(uname -s)" in
    Linux)   os="linux" ;;
    Darwin)  os="macos" ;;
    MINGW*|MSYS*|CYGWIN*) os="windows" ;;
    *) die "不认识的系统 $(uname -s)。自己编：见 cpp/README.md" ;;
  esac
  case "$(uname -m)" in
    x86_64|amd64)  arch="x64" ;;
    aarch64|arm64) arch="arm64" ;;
    *) die "不认识的架构 $(uname -m)。自己编：见 cpp/README.md" ;;
  esac
  echo "${os}-${arch}"
}

# ---- 依赖检查 ----

check_ffmpeg() {
  if command -v ffmpeg >/dev/null 2>&1 && command -v ffprobe >/dev/null 2>&1; then
    return 0
  fi
  info "没有找到 ffmpeg，尝试安装（装配环节的硬依赖）"
  install_pkgs ffmpeg || {
    warn "自动安装 ffmpeg 失败。装配环节会用不了。"
    warn "请手动安装后重跑 changji --doctor。"
    return 1
  }
}

check_fonts() {
  # 中文字幕烧录需要中文字体，缺了会渲染成方框
  if fc-list 2>/dev/null | grep -qiE "noto sans cjk|source han sans|wqy|pingfang"; then
    return 0
  fi
  # macOS 自带苹方，fc-list 未必装，别在这儿吓唬人
  if [ "$(uname -s)" = "Darwin" ]; then return 0; fi
  info "没有找到中文字体，尝试安装（字幕烧录需要）"
  install_pkgs fonts-noto-cjk 2>/dev/null \
    || install_pkgs google-noto-sans-cjk-fonts 2>/dev/null \
    || install_pkgs noto-fonts-cjk 2>/dev/null \
    || warn "自动安装中文字体失败。中文字幕可能显示为方框。"
}

# ---- 取二进制 ----

download() {
  local url="$1" out="$2"
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL --retry 3 -o "$out" "$url"
  elif command -v wget >/dev/null 2>&1; then
    wget -q -O "$out" "$url"
  else
    die "既没有 curl 也没有 wget，装一个再来"
  fi
}

# **校验和要验。** 不验的话，半路断掉的下载会变成一个"能装上、
# 一运行就 Exec format error"的文件，而那个报错和网络问题看不出关系。
verify_sha256() {
  local file="$1" sums="$2" name="$3"
  local want got
  want="$(awk -v n="$name" '$2 == n || $2 == "*"n {print $1}' "$sums" | head -n1)"
  if [ -z "$want" ]; then
    warn "SHA256SUMS 里没有 $name，跳过校验"
    return 0
  fi
  if command -v sha256sum >/dev/null 2>&1; then
    got="$(sha256sum "$file" | awk '{print $1}')"
  elif command -v shasum >/dev/null 2>&1; then
    got="$(shasum -a 256 "$file" | awk '{print $1}')"
  else
    warn "没有 sha256sum / shasum，跳过校验"
    return 0
  fi
  [ "$want" = "$got" ] || die "校验和对不上（想要 $want，拿到 $got），重下一次"
}

fetch_release() {
  local target asset base tmp
  target="$(detect_target)"
  asset="changji-${target}.tar.gz"
  if [ "$VERSION" = "latest" ]; then
    base="https://github.com/${REPO}/releases/latest/download"
  else
    base="https://github.com/${REPO}/releases/download/${VERSION}"
  fi

  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' EXIT

  info "下载 $asset（$VERSION）"
  download "${base}/${asset}" "$tmp/$asset" \
    || die "下载失败：${base}/${asset}
这个平台可能没出包，或者版本号写错了。已出的包见
  https://github.com/${REPO}/releases"

  if download "${base}/SHA256SUMS" "$tmp/SHA256SUMS" 2>/dev/null; then
    verify_sha256 "$tmp/$asset" "$tmp/SHA256SUMS" "$asset"
    info "校验和通过"
  else
    warn "取不到 SHA256SUMS，跳过校验"
  fi

  tar -xzf "$tmp/$asset" -C "$tmp"
  [ -f "$tmp/changji" ] || die "包里没有 changji 这个文件，包可能是坏的"
  mkdir -p "$PREFIX/bin"
  install -m 755 "$tmp/changji" "$PREFIX/bin/changji"
}

# ---- 安装 ----

main() {
  info "安装位置 $PREFIX"

  check_ffmpeg || true
  check_fonts || true

  if [ -n "${CHANGJI_BINARY:-}" ]; then
    [ -f "$CHANGJI_BINARY" ] || die "CHANGJI_BINARY 指的文件不存在：$CHANGJI_BINARY"
    info "用本地二进制 $CHANGJI_BINARY"
    mkdir -p "$PREFIX/bin"
    install -m 755 "$CHANGJI_BINARY" "$PREFIX/bin/changji"
  else
    fetch_release
  fi

  # 放一个软链到 PATH 上常见的位置
  local bindir="$HOME/.local/bin"
  mkdir -p "$bindir"
  ln -sf "$PREFIX/bin/changji" "$bindir/changji"

  info "装的是 $("$PREFIX/bin/changji" --version 2>/dev/null || echo '一个二进制')"
  info "安装完成"
  echo
  if ! echo ":$PATH:" | grep -q ":$bindir:"; then
    warn "$bindir 不在 PATH 里。把下面这行加进你的 shell 配置："
    echo "    export PATH=\"\$HOME/.local/bin:\$PATH\""
    echo
  fi
  echo "下一步："
  echo "    changji --doctor           检查环境，缺什么它会说"
  echo "    changji --port 8080        起服务，浏览器打开 http://127.0.0.1:8080"
  echo
  echo "第一次打开会先让你选模型下载——出图、出片、写剧本、配音各要一份权重，"
  echo "在界面上选完它自己下。想用别的机器上的大模型就在设置页填地址。"
}

main "$@"
