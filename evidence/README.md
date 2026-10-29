# 实测证据

所有文件都是在本机（Quectel Pi M2 / RK3576 / Debian 13）上跑出来的原始输出，不是手写。

| 文件 | 内容 | 关键结论 |
|---|---|---|
| `01-ui-fullscreen.png` | 1280×800 全屏界面截图（离线送帧模式下，画面里有一人且被判为「歪头」） | 骨架用红色绘制、关节点黄色、右面板状态红字，说明「引擎→规则→界面」整条链路通 |
| `verify-output.txt` | `scripts/verify.sh` 16 项自检 | **PASS=16 FAIL=0 WARN=0** |
| `board-check.txt` | `scripts/check_board.sh` 环境体检 | PASS=15 WARN=1（`/dev/rknpu` 节点不存在，驱动编进内核，不影响） |
| `unit-tests.txt` | `ctest` 三个测试 | **100% tests passed, 0 failed out of 3** |
| `m2-compare-onnx.txt` | 与 ONNX fp32 参考逐点比对（zidane / bus / 现场真实帧） | 三例全部 **PASS**，关键点平均误差 **17.2 / 17.2 / 12.0 px**，匹配率 100% |
| `metrics-live.txt` | 现场连续采样 6 次（2 秒一次） | NPU 17–19ms/帧、6.6–6.9 fps、`keypoints_valid` 随是否有人正确翻转 |
| `status-sample.json` | 一帧完整状态快照（socket 原始 JSON） | 字段齐全，可直接给 UI 或外部消费 |
| `focus-measurements.txt` | 焦距扫描 / AF 对比 / A-B 实测原始数据 | 锁定 70 相对设备默认 160 清晰度 **1.87×** |
| `03-ui-focus-locked.png` | 锁定焦距后的界面截图 | 画面区 Laplacian 555、160923 色 |
| `04-ui-topdown-camera.png` | 俯视机位（摄像头在显示器上方）下的界面截图 | 画面区 179817 色 |
| `topdown-camera-fixes.txt` | 俯视机位三连问题的完整实测数据与修复记录 | 误报清零、校准 28 帧一次通过 |

## 关键数字

```
NPU 单帧推理        17.0 – 18.6 ms      （ONNX CPU 同模型 331–401ms，快 16–19×）
推理帧率            6.6 – 6.9 fps       （配置上限 camera.infer_fps: 8）
关键点 vs ONNX      12.0 – 17.2 px 平均 （w8a8 量化的固有误差）
人体框 IoU          0.333 – 0.876      （zidane 主目标 0.876）
校准基准           肩线 4.5°，头肩纵向 -0.897（23 帧中位数，离群过滤后）
采集              640×480 YUYV（该 UVC 不提供 MJPG，日志有 WARN，不影响功能）
```

## 判定链路实测

`metrics-live.txt` 里同一段连续采样：

```
present=True  state=HEAD_DOWN  dev=4.5  head_oy=-0.353  wr=0.085  w2n=1.46  lat=17.2ms kp=True
present=True  state=HEAD_DOWN  dev=4.5  head_oy=-0.471  wr=0.085  w2n=2.01  lat=17.0ms kp=True
present=False state=USER_AWAY  dev=0.0  head_oy= 0.000  wr=0.000  w2n=0.00  lat=18.4ms kp=False
```

* `head_oy=-0.353` 相对基准 `-0.897` 下降了 0.54 肩宽 → 超过 `head_down_drop: 0.35`
  → 判「低头/趴桌」，持续 5s 后提醒；
* 人走开后同一进程立刻变 `USER_AWAY`、`keypoints_valid=false`，不会拿旧骨架继续判；
* 全程 `latency_ms` 稳定在 17–19ms。

## 复现命令

```bash
# 自检
adb shell 'bash /opt/posture-guard/scripts/verify.sh'
# 环境体检
adb shell 'bash /opt/posture-guard/scripts/check_board.sh'
# 单元测试
adb shell 'cd /userdata/build-src && ctest --test-dir build --output-on-failure'

# M2 验收（静态图 vs ONNX）
adb shell '/opt/posture-guard/bin/posture_guard --config /etc/posture-guard/posture.yaml \
  --image /userdata/build-src/zidane.jpg --dump-keypoints /tmp/z.json'
adb pull /tmp/z.json . && python3 tests/compare_onnx.py samples/zidane.jpg z.json

# 界面截图（GNOME Wayland 下 grim/xwd 都不可用，只有 kmsgrab 可行）
adb shell 'bash /opt/posture-guard/scripts/screenshot.sh /tmp/s.png' && adb pull /tmp/s.png .

# 实时指标
adb shell 'bash /opt/posture-guard/scripts/status.sh --watch'
```

## 取证注意

* 截图必须和 `status.json` 一起存，否则无法证明「截图里那个状态是真的」。
* `--source-image` 离线送帧模式**只用于验证渲染与链路**，不能当现场验收证据。
* 现场验收要留下：`verify.sh` 输出 + `journalctl -u posture-guard` 片段 +
  截图 + 当时的 `status.json`。

## 摄像头画面（2026-10-28 补）

| 文件 | 内容 |
|---|---|
| `01-ui-fullscreen.png` | 现场实时画面 + 骨架叠加（画面区 108806 色，骨架红 4227px、关节点黄 590px） |
| `02-ui-offline-source-image.png` | `--source-image` 离线送帧模式，验证视频通道不依赖摄像头 |

判定"画面真的在跑"的方法（不靠肉眼）：

```bash
# 1) 画面区必须是照片纹理，不是纯色块
python3 -c "
import cv2,numpy as np
im=cv2.imread('evidence/01-ui-fullscreen.png')[:, :900]
print('颜色数', len(np.unique(im.reshape(-1,3),axis=0)))"   # 108806

# 2) socket 上必须有 video 消息且 seq 在增长
python3 - <<'PY'
import socket, json
s=socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect('/run/posture-guard/status.sock')
buf=b''
while b'\n' not in buf: buf += s.recv(65536)
d=json.loads(buf.split(b'\n')[0]); print(d['type'], d.get('seq'), d.get('w'), d.get('h'))
PY
```

`verify.sh` 里已经把这条做成硬检查（「视频流」一项），没有画面就是 FAIL。

**证据配对**：截图必须与同一时刻的 `status-sample.json` 一起存。当前这一对是
`posture_state_zh=正常 / keypoints_valid=true / persons 原始 2 → 去重后 1`。
