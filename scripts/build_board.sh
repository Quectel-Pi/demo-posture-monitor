#!/usr/bin/env bash
# 板端原生编译 + 单元测试（对应实施方案 §8.3 的编译步骤）
#
# 用法：bash scripts/build_board.sh [--clean]
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."
BUILD_DIR="${BUILD_DIR:-build}"

command -v cmake >/dev/null || { echo "!! 缺 cmake：先跑 scripts/fetch_deps.sh 并安装 deb" >&2; exit 1; }
command -v g++   >/dev/null || { echo "!! 缺 g++：先跑 scripts/fetch_deps.sh 并安装 deb" >&2; exit 1; }

if [[ "${1:-}" == "--clean" ]]; then
  echo "== 清理 $BUILD_DIR =="
  rm -rf "$BUILD_DIR"
fi

echo "== 配置 =="
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release 2>&1 | sed 's/^/   /'

echo "== 编译（-j$(nproc)）=="
cmake --build "$BUILD_DIR" -j"$(nproc)" 2>&1 | sed 's/^/   /'

echo "== 单元测试 =="
ctest --test-dir "$BUILD_DIR" --output-on-failure 2>&1 | tail -12

echo
ls -l "$BUILD_DIR/posture_guard"
file "$BUILD_DIR/posture_guard" | sed 's/^/  /'
