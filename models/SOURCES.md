# 模型来源与许可

| 文件 | 来源 | 用途 | 备注 |
|---|---|---|---|
| `yolov8n-pose-rknn.rknn` | https://huggingface.co/RKNNAI/RK3576-CNN-yolov8n-pose | **主用（NPU）** | w8a8, 640×640, RKNN Runtime 2.4.0 编译；板端 librknnrt 2.3.2 实测可加载 |
| `yolov8n-pose.onnx` | https://huggingface.co/Xenova/yolov8n-pose | 备援（CPU） | fp32，输出 `(1,56,8400)` |
| `yolov8n-pose-int8.onnx` | 同上 | 留档 | 实测 CPU 上比 fp32 更慢（446ms vs 330ms），未启用 |

均为 YOLOv8n-pose（COCO-17 关键点），输入 640×640 letterbox。
上游许可：Ultralytics AGPL-3.0（模型页另有声明）。商业交付前请按公司政策复核。
重新获取：`bash scripts/fetch-model.sh`
