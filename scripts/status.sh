#!/usr/bin/env bash
# 从状态 socket 读一帧状态（薄 wrapper，实际逻辑在 probe_status.py）
#
# 用法：
#   bash scripts/status.sh              # 人类可读
#   bash scripts/status.sh --raw        # 原始 JSON
#   bash scripts/status.sh --watch      # 每秒刷新
#
# 为什么只是 wrapper：socket 上混着 status/video/notification 三类消息，
# 而且要输出带 f-string 的中文，在 shell heredoc 里转义会变成灾难。
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "$HERE/probe_status.py" "$@"
