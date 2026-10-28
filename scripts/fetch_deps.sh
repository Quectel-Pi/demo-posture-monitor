#!/usr/bin/env bash
# 在主机（x86_64 + 代理）上准备板端编译所需的离线依赖
#
# 板端 Debian 13 没有任何编译器，也没有外网。这个脚本在**主机**上跑，
# 从清华源把 Debian trixie arm64 的编译器 deb 下下来，连同一个
# rknn_api.h 一起打成 tools/board-deps.tgz，推到板端 dpkg -i 即可。
#
# 用法：
#   bash scripts/fetch_deps.sh                 # 自动探测是否需要工具链
#   bash scripts/fetch_deps.sh --with-toolchain # 强制下载工具链
#   PROXY=http://127.0.0.1:7897 bash scripts/fetch_deps.sh
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
PROJ="$(pwd)"
OUT_DIR="${OUT_DIR:-$PROJ/tools}"
PROXY="${PROXY:-http://127.0.0.1:7897}"
MIRROR="${MIRROR:-https://mirrors.tuna.tsinghua.edu.cn/debian}"
ADB_TARGET="${ADB_TARGET:-}"      # 例如 "-s 12345678"，留空表示默认设备
WITH_TOOLCHAIN=0
[[ "${1:-}" == "--with-toolchain" ]] && WITH_TOOLCHAIN=1

mkdir -p "$OUT_DIR"
export http_proxy="$PROXY" https_proxy="$PROXY"

adb_() { adb ${ADB_TARGET:+$ADB_TARGET} "$@"; }

have_compiler=0
if adb_ shell 'command -v g++ >/dev/null && command -v cmake >/dev/null' >/dev/null 2>&1; then
  have_compiler=1
fi
echo "板端已有 g++/cmake: $have_compiler"

# ---------------------------------------------------------------------------
# 1. rknn_api.h（必须与板端 librknnrt.so 主版本一致）
# ---------------------------------------------------------------------------
RKNN_REF="${RKNN_REF:-v2.3.2}"
if [[ -f "$PROJ/vendor/rknn/rknn_api.h" ]]; then
  echo "[1/3] rknn_api.h 已存在，跳过"
else
  echo "[1/3] 下载 rknn_api.h ($RKNN_REF)"
  mkdir -p "$PROJ/vendor/rknn"
  curl -fsSL -o "$PROJ/vendor/rknn/rknn_api.h" \
    "https://raw.githubusercontent.com/airockchip/rknn-toolkit2/$RKNN_REF/rknpu2/runtime/Linux/librknn_api/include/rknn_api.h"
fi
board_ver="$(adb_ shell 'strings /usr/lib/librknnrt.so 2>/dev/null | grep -oE "^2\.[0-9]+\.[0-9]+" | head -1' | tr -d '\r')"
echo "      板端 runtime 版本: ${board_ver:-未知}（头文件取自 $RKNN_REF）"
if [[ -n "$board_ver" && "$RKNN_REF" != "v$board_ver" && "$RKNN_REF" != "rknn$board_ver" ]]; then
  echo "      ⚠ 版本号不一致，若 rknn_init 报错请换成 v$board_ver 的头文件"
fi

# ---------------------------------------------------------------------------
# 2. 模型（仓库里已有，缺了才提示手工放）
# ---------------------------------------------------------------------------
if [[ -f "$PROJ/models/yolov8n-pose.rknn" ]]; then
  echo "[2/3] 模型已存在 models/yolov8n-pose.rknn"
elif [[ -f "$PROJ/models/yolov8n-pose-rknn.rknn" ]]; then
  cp "$PROJ/models/yolov8n-pose-rknn.rknn" "$PROJ/models/yolov8n-pose.rknn"
  echo "[2/3] 已把 yolov8n-pose-rknn.rknn 复制为 yolov8n-pose.rknn"
else
  echo "[2/3] !! models/ 下没有 .rknn 模型。" >&2
  echo "      按实施方案 §9 在 x86 主机上两步量化生成，或从交付包 models/ 拷贝。" >&2
fi

# ---------------------------------------------------------------------------
# 3. 板端工具链 deb（仅在缺编译器时）
# ---------------------------------------------------------------------------
if [[ $have_compiler -eq 1 && $WITH_TOOLCHAIN -eq 0 ]]; then
  echo "[3/3] 板端已有编译器，跳过工具链下载"
  exit 0
fi

echo "[3/3] 下载 Debian trixie arm64 工具链 deb（g++-14 / cmake / make）"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"/{lists/partial,cache/archives/partial,debs}
adb_ pull /var/lib/dpkg/status "$TMP/status" >/dev/null
cat > "$TMP/sources.list" <<EOF
deb [arch=arm64] $MIRROR trixie main
EOF
APT_OPTS=(
  -o "Dir::Etc::sourcelist=$TMP/sources.list"
  -o "Dir::Etc::sourceparts=-"
  -o "Dir::State::status=$TMP/status"
  -o "Dir::State::lists=$TMP/lists"
  -o "Dir::Cache=$TMP/cache"
  -o "APT::Architecture=arm64"
  -o "APT::Architectures::=arm64"
  -o "Acquire::https::Proxy=$PROXY"
  -o "Acquire::AllowInsecureRepositories=true"
  -o "APT::Get::AllowUnauthenticated=true"
)
apt-get "${APT_OPTS[@]}" update -qq
apt-get "${APT_OPTS[@]}" -y --download-only --no-install-recommends \
  -o "Dir::Cache::archives=$TMP/debs" install g++-14 cmake make >/dev/null
n=$(ls "$TMP"/debs/*.deb | wc -l)
echo "      拿到 $n 个 deb，共 $(du -sh "$TMP/debs" | cut -f1)"
tar czf "$OUT_DIR/board-deps.tgz" -C "$TMP/debs" .
echo "      -> $OUT_DIR/board-deps.tgz"
cat > "$OUT_DIR/README.md" <<'EOF'
# tools/board-deps.tgz

板端离线编译依赖（Debian 13 trixie / arm64）：
`g++-14` `cmake` `make` `binutils` `libstdc++-14-dev` 以及它们的依赖。

安装：
```bash
adb push tools/board-deps.tgz /userdata/
adb shell 'mkdir -p /userdata/board-deps && cd /userdata/board-deps && \
  tar xzf ../board-deps.tgz && dpkg -i --force-depends *.deb && \
  update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100 \
    --slave /usr/bin/g++ g++ /usr/bin/g++-14'
```

> 只用于开发/演示阶段。正式量产应改用交叉编译 + 只把二进制推到板端。
EOF
echo "完成。下一步：adb push $OUT_DIR/board-deps.tgz /userdata/ 然后在板端安装"
