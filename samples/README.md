# 验证用样例

| 文件 | 用途 | 来源 |
|---|---|---|
| `bus.jpg` | 4 人（站立/坐姿混杂），用于验证多目标 + 关键点精度 | ultralytics 官方样图（COCO 图片） |
| `zidane.jpg` | 2 人半身，用于验证大目标关键点 | 同上 |

用法（板端）：

```bash
PYTHONPATH=/opt/posture-monitor python3 -m posturemon.headless \
    --source /opt/posture-monitor/samples/zidane.jpg --seconds 5
```

这两张图只是**离线回归**用，与真实坐姿判定无关；真实判定阈值请用真人现场标定，
方法见 `docs/03-posture-rules.md`。
