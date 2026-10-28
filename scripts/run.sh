#!/bin/bash
# run.sh — 前台运行（调试用，直接看 stdout）
export PYTHONPATH=/opt/posture-monitor
export PYTHONUNBUFFERED=1
exec python3 -m posturemon "$@"
