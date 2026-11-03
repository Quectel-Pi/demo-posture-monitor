# 模型来源与许可

| 文件 | 来源 | 用途 | 备注 |
|---|---|---|---|
| `yolov8n-pose.rknn` | https://huggingface.co/RKNNAI/RK3576-CNN-yolov8n-pose | **主用（NPU）** | w8a8, 640×640, RKNN Runtime 2.4.0 编译；板端 librknnrt 2.3.2 实测可加载 |
| `yolov8n-pose.onnx` | https://huggingface.co/Xenova/yolov8n-pose | 备援（CPU）/ 解码对照基准 | fp32，输出 `(1,56,8400)` |

均为 YOLOv8n-pose（COCO-17 关键点），输入 640×640 letterbox。
上游许可：Ultralytics AGPL-3.0（模型页另有声明）。商业交付前请按公司政策复核。

## 未入库的模型

以下文件**不再随仓库分发**，需要时按来源自行获取：

| 文件 | 为什么不入库 |
|---|---|
| `yolov8n-pose-rknn.rknn` | 与 `yolov8n-pose.rknn` 逐字节相同（同一文件两个名字），已去重 |
| `yolov8n-pose-int8.onnx` | 实测 CPU 上比 fp32 更慢（446ms vs 330ms），未启用 |
