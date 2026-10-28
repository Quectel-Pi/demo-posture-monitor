# 04 · 模型与推理后端

## 1. 用的模型

| 文件 | 来源 | 输入 | 输出 | 用途 |
|---|---|---|---|---|
| `yolov8n-pose-rknn.rknn` | [RKNNAI/RK3576-CNN-yolov8n-pose](https://huggingface.co/RKNNAI/RK3576-CNN-yolov8n-pose) | 640×640 NHWC **uint8** | 4 个 tensor（见下） | 主用，NPU |
| `yolov8n-pose.onnx` | [Xenova/yolov8n-pose](https://huggingface.co/Xenova/yolov8n-pose) | 640×640 NCHW float32/255 | `(1,56,8400)` | 备援，CPU |
| `yolov8n-pose-int8.onnx` | 同上 | 同上 | 同上 | 留档（实测更慢，未启用） |

都是标准 YOLOv8n-pose，COCO-17 关键点顺序：
`nose, eye_l, eye_r, ear_l, ear_r, shoulder_l, shoulder_r, elbow_l, elbow_r,
wrist_l, wrist_r, hip_l, hip_r, knee_l, knee_r, ankle_l, ankle_r`

## 2. RKNN 模型的 4 个输出（这里最容易踩坑）

```
out[0] (1,65,80,80)     stride 8   64 ch DFL 距离 + 1 ch 分类 logits（未 sigmoid）
out[1] (1,65,40,40)     stride 16  同上
out[2] (1,65,20,20)     stride 32  同上
out[3] (1,17,3,8400)    17 关键点 × (x, y, conf)，按 anchor 排列
```

解码要点（`yolopose.decode_rknn()`）：

1. **布局是 NCHW**，即 `reshape(65, h, w)`。`rknn_query` 报的 `fmt=NCHW` 是可信的。
   （我一开始按 NHWC 试过，conf 通道全是噪声 —— 用 ONNX 结果做相关性比对才定下来。）
2. **分类通道要 sigmoid**，输出是 logits。
3. **DFL 解码**：把 64 通道按 `(4, 16)` reshape，对 16 个 bin 做 softmax 再取期望值，
   这才是 ltrb 距离。
4. **关键点张量按 anchor 索引**：`kk = out[3].reshape(17,3,8400)`，
   取 `kk[:, :, anchor]` 再 `transpose(2,0,1)` → `(N,17,3)`。
5. **关键点 conf 不可用**：整个 51 通道张量共用一套量化参数（scale≈3.19），
   0~1 的置信度只剩 1 个量化级 —— 实测该通道恒为 0。
   所以关联方式是"取该框对应 anchor 的关键点"，而不是"按 conf 过滤"。

验证方式（`tests/verify_decode.py`）：同一张图同时跑 RKNN 与 ONNX fp32，
按 IoU 配对后逐点比坐标。实测平均误差 **4.9–6.9 px**（640 空间），
说明解码完全正确，差异只来自 w8a8 量化。

## 3. 为什么 NPU 用 ctypes 直调

板端条件：

- 有 `/usr/lib/librknnrt.so`（2.3.2）和 RKNPU 驱动（0.9.8）
- **没有 pip**，`python3 -m pip` 直接报 no module
- python 3.13，`rknn-toolkit-lite2` 没有对应 wheel
- 板端**无外网**

而 `rknn_api.h` 里跑推理只需要 5 个结构体 + 5 个函数：

```c
rknn_init(); rknn_query(); rknn_inputs_set(); rknn_run();
rknn_outputs_get(); rknn_outputs_release(); rknn_destroy();
```

用 `ctypes.Structure` 照着头文件摆一遍就能用，零安装、零编译。
实现见 `backends.RknnBackend`（结构体定义在文件顶部）。

踩过的两个细节：

- `rknn_tensor_type`：`INT8=2`、**`UINT8=3`**。写错不会报错，只会输出垃圾。
- 模型是 toolkit 2.4.0 编译的，板端 runtime 只有 2.3.2 —— **实测能加载**，
  `rknn_init` 返回 0，无版本拒载。如果换模型后加载失败，先查这个版本差。

## 4. 性能实测

| 后端 | 单帧耗时 | 说明 |
|---|---|---|
| **RKNN w8a8 @NPU** | **18–25 ms** | 稳定，与 ONNX 对照点误差 5–7px |
| ONNX fp32 @CPU 8 线程 | 353 ms | 默认 |
| ONNX fp32 @CPU 6 线程 | **315 ms** | 最优（线程再多反而退化） |
| ONNX fp32 @CPU 4 线程 | 392 ms | |
| ONNX int8 @CPU | 446 ms | 量化解量化开销大于收益，**更慢** |

结论：`infer_interval_ms=80` 时 NPU 占用非常轻（~25/80），CPU 备援模式建议
`infer_interval_ms=400` 并接受 ~2.5fps 的判定节奏（姿态变化慢，够用）。

## 5. 后端切换

```python
# config.json
"backend": "rknn"   # rknn | onnx | auto（auto = 先 rknn，失败退 onnx）
```

`make_backend()` 会把失败原因收集起来抛在异常里，例如：

```
RuntimeError: 没有可用的推理后端；rknn: FileNotFoundError: .../yolov8n-pose-rknn.rknn
```

强制对比演示（现场展示端侧算力差距）：

```bash
POSTURE_ARGS="--backend=onnx" sudo -u quectel /usr/local/bin/posture-monitor.sh
```

命令行覆盖任意配置项：`--backend=onnx --conf_threshold=0.5`（`app.main()` 解析）。

## 6. 接 PeopleKptDet SDK（后续）

接口契约：

```python
class SomeBackend:
    name = "peoplekpt"
    in_h = in_w = 640            # 模型输入边长
    sdk = "PeopleKptDet v1.2"    # 显示在面板

    def infer(self, rgb: np.ndarray, conf_th=0.45, iou_th=0.45) -> PoseResult:
        """rgb: (in_h, in_w, 3) uint8 RGB。
        返回 PoseResult(boxes(N,4 xywh), scores(N,), kpts(N,17,3),
                        latency_ms, backend_name)"""
```

然后在 `make_backend()` 的候选列表里把它排到最前即可，其余代码零改动。
