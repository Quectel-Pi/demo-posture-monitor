#!/bin/bash
# fetch-model.sh — 在**开发机**上准备模型与 ONNX 备援运行时（板子没外网）。
#
#   bash scripts/fetch-model.sh [--no-ort]
#
# 产出：
#   models/yolov8n-pose-rknn.rknn    RK3576 NPU w8a8（主用，~18ms/帧）
#   models/yolov8n-pose.onnx         fp32 CPU 备援（~330ms/帧）
#   models/yolov8n-pose-int8.onnx    量化备援（实测反而更慢，仅留档）
#   dist/onnxruntime*.whl            aarch64 / cp3.13 wheel（板端 pip 缺失时解压即用）
#   models/LICENSE-*.txt             来源与许可
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODELS="$REPO/models"
DIST="$REPO/dist"
NO_ORT=0
[ "${1:-}" = "--no-ort" ] && NO_ORT=1

PROXY="${HTTPS_PROXY:-${https_proxy:-}}"
CURL=(curl -sSL --retry 3 --retry-delay 2)
[ -n "$PROXY" ] && CURL+=(--proxy "$PROXY")

mkdir -p "$MODELS" "$DIST"
log() { echo -e "\033[1;34m[*]\033[0m $*"; }
ok()  { echo -e "\033[1;32m[+]\033[0m $*"; }

log "RKNN 模型（RKNNAI/RK3576-CNN-yolov8n-pose, w8a8, 640x640）"
"${CURL[@]}" -o "$MODELS/yolov8n-pose-rknn.rknn" \
  "https://huggingface.co/RKNNAI/RK3576-CNN-yolov8n-pose/resolve/main/yolov8n-pose-640x640-w8a8-1/yolov8n-pose.rknn"
ok "$(du -h "$MODELS/yolov8n-pose-rknn.rknn" | cut -f1)"

log "ONNX 备援（Xenova/yolov8n-pose）"
"${CURL[@]}" -o "$MODELS/yolov8n-pose.onnx" \
  "https://huggingface.co/Xenova/yolov8n-pose/resolve/main/onnx/model.onnx"
"${CURL[@]}" -o "$MODELS/yolov8n-pose-int8.onnx" \
  "https://huggingface.co/Xenova/yolov8n-pose/resolve/main/onnx/model_int8.onnx"
ok "$(du -h "$MODELS/yolov8n-pose.onnx" | cut -f1)"

log "记录来源"
cat > "$MODELS/SOURCES.md" <<'EOF'
# 模型来源

| 文件 | 来源 | 用途 | 备注 |
|---|---|---|---|
| `yolov8n-pose-rknn.rknn` | https://huggingface.co/RKNNAI/RK3576-CNN-yolov8n-pose | 主用（NPU） | w8a8, 640x640, RKNN Runtime 2.4.0 编译；板端 librknnrt 2.3.2 实测可加载 |
| `yolov8n-pose.onnx` | https://huggingface.co/Xenova/yolov8n-pose | 备援（CPU） | fp32, 输出 (1,56,8400) |
| `yolov8n-pose-int8.onnx` | 同上 | 留档 | 实测 CPU 上比 fp32 更慢（446ms vs 330ms），未启用 |

两者都是 YOLOv8n-pose（COCO-17 关键点），输入 640x640 letterbox，许可沿用上游
Ultralytics AGPL-3.0 / 模型页声明。商业交付前请按公司政策复核。
EOF
ok "SOURCES.md"

if [ "$NO_ORT" = 0 ]; then
    log "ONNX Runtime aarch64 wheel（板端无 pip 时解压到 PYTHONPATH 即可）"
    WHL="onnxruntime-1.31.1-cp313-cp313-manylinux_2_28_aarch64.whl"
    URL=$(python3 - <<'PY'
import json, sys, urllib.request
try:
    d = urllib.request.urlopen("https://pypi.org/pypi/onnxruntime/json", timeout=20).read()
    j = json.loads(d)
    for u in j["urls"]:
        if "cp313" in u["filename"] and "aarch64" in u["filename"] and "cp313t" not in u["filename"]:
            print(u["url"]); break
except Exception as e:
    sys.stderr.write(f"{e}\n")
PY
)
    [ -n "$URL" ] || URL="https://files.pythonhosted.org/packages/py3/o/onnxruntime/$WHL"
    "${CURL[@]}" -o "$DIST/$WHL" "$URL"
    ok "dist/$WHL ($(du -h "$DIST/$WHL" | cut -f1))"
fi

echo
log "校验"
ls -la "$MODELS" | sed 's/^/  /'
echo
log "下一步：bash scripts/deploy-from-host.sh"
