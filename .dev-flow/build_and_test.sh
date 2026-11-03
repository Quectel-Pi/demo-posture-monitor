#!/usr/bin/env bash
# 板端 68 统一编译 + 单测入口（lead 专用，串行执行）
#
# 为什么需要它：主机没有 OpenCV 头文件，板端 66 没有编译器，
# 唯一能编译的位置是板端 68（生产机）。为避免多个 agent 同时编译打架，
# 所有编译/单测都由 lead 通过本脚本串行执行。
#
# 用法：bash .dev-flow/build_and_test.sh [--clean]
set -uo pipefail

SRC=/home/leox/repositories/PostureMonitor
BOARD_ID="${BOARD:-2}"
ADB="adb -t ${BOARD_ID}"
REMOTE=/userdata/pm-build

say() { printf '\n=== %s ===\n' "$*"; }

say "0. 预检"
$ADB shell 'command -v g++ cmake' >/dev/null || { echo "板端缺编译器"; exit 1; }

say "1. 同步源码到板端 $REMOTE"
$ADB shell "rm -rf $REMOTE && mkdir -p $REMOTE"
# 只推送编译需要的部分，避免把 .git / evidence 大目录推过去
tar -C "$SRC" -czf - \
    CMakeLists.txt include src tests config models vendor scripts assets ui \
    2>/dev/null | $ADB shell "tar xzf - -C $REMOTE"
$ADB shell "ls $REMOTE" | head -20

say "2. 编译"
CLEAN_FLAG=""
[[ "${1:-}" == "--clean" ]] && CLEAN_FLAG="--clean"
$ADB shell "cd $REMOTE && BUILD_DIR=build bash scripts/build_board.sh $CLEAN_FLAG" 2>&1 | tail -40

say "3. 单测明细"
$ADB shell "cd $REMOTE && for t in build/test_posture_rules build/test_session_timer build/test_config build/test_profile_hysteresis; do
  [ -x \$t ] || continue
  echo \"---- \$t ----\"
  if [ \$t = build/test_config ]; then \$t config/posture.yaml 2>&1 | tail -8; else \$t 2>&1 | tail -8; fi
  echo \"exit=\$?\"
done"

say "完成"
