# 05 · 排障记录（本次实际踩到的坑）

按"症状 → 定位 → 根因 → 修法"写，方便下次遇到同类问题直接查。

---

## 1. 推理线程里 segfault，无 traceback

**症状**：`verify.sh` 端到端项偶发 `Segmentation fault`，`faulthandler` 显示崩溃在
`backends.py infer()` 里，栈顶是 `numpy/__init__.py` 的 `__getattr__`。

**根因**：numpy 2.x 把 `np.ctypeslib` 做成了**惰性属性**。首次访问发生在推理线程，
同时采集线程在跑 —— 两个线程并发 import 子模块，CPython 的 import 锁救不了，
直接段错误。

**修法**（双保险）：

1. `backends.py` 顶部显式 `import numpy.ctypeslib`（主线程完成 import）
2. `Pipeline.__init__` 里先用一张全零图 `backend.infer()` 预热一次

**教训**：任何"懒加载"的库都不要在线程里第一次用。

---

## 2. 程序莫名 `Aborted`，日志只有一句 cairo 断言

**症状**：

```
python3: ../../../src/cairo-surface.c:1739: cairo_surface_mark_dirty_rectangle:
Assertion `! _cairo_surface_has_snapshots (surface)' failed. Aborted
```

**根因**：为了省事复用了同一个 `cairo.ImageSurface`（`create_for_data` 包住固定 numpy
buffer），每帧 `mark_dirty()`。但 GTK4 的 `DrawingArea` draw func 会保留快照
（recording surface），之后再改动同一块像素就等于改已发布的表面 → cairo 直接 abort。

**修法**：环形缓冲（4 块）+ **每帧新建 surface**，不调 `mark_dirty()`。
GTK 保留最近几帧的 buffer 引用也没关系，反正下一帧用的是另一块。

**教训**：交给 GTK 的像素缓冲，当它已经是"不可变"的。

---

## 3. 界面不刷新、`status.json` 不生成、进程还活着

**症状**：应用在跑，但状态面板不更新，`/run/posture-monitor/status.json` 一直不存在，
DB 里 0 行。用 `faulthandler.dump_traceback_later(5, repeat=True)` 才看到卡在：

```
stats.py:59 flush()  ←  stats.py:102 today()  ←  window.py:370 _tick_panel()
```

**根因**：`Stats.today()` 先拿 `self._lock`，再调 `self.flush()`，而 `flush()` 又去拿同一把
**非重入** `threading.Lock` → 自锁。

**修法**：`threading.RLock()`。

**教训**：凡是"查询接口内部会触发写/刷新"的类，一律用 RLock。

---

## 4. RKNN 输出全是垃圾（解码第一步就错）

**症状**：conf 通道全负、关键点坐标是 9.57 的整数倍、框满天飞。

**排查过程**（值得复用）：拿同一张图跑 ONNX fp32 拿到"正确答案"，
再对 RKNN 的每个输出试各种 reshape，逐通道算相关系数：

```
NCHW ch64   corr +0.501  ← 对的（80×80 尺度上 conf 本来就很低，弱相关正常）
NHWC ch64   corr +0.027
```

**根因**：`rknn_query` 报 `fmt=NCHW` 是对的，我却先按 NHWC 试了；另外
`rknn_input` 的 `type` 用了 `2`（=INT8），应该用 **`3`（=UINT8）**。

**修法**：`reshape(65, h, w)`；`type=3`。

---

## 5. 关键点张量索引错，取出来全是 0

**症状**：`kpt_conf` 恒为 0，坐标对不上。

**根因**：`out[3]` 形状 `(1,17,3,8400)`，取某 anchor 的关键点要
`kk[:, :, anchor]` 再 `transpose(2,0,1)`。我写成了 `transpose(1,0,2)`，
得到 `(3,17,N)`，读出来的当然不是坐标。

顺带发现：**conf 通道在这个 w8a8 模型里被量化摧毁了**（整个 51 通道共用一个
scale≈3.19 的量化参数，0~1 的置信度只剩一个量化级）。所以不能按 conf 过滤关键点，
必须"跟着框的 anchor 走"。

**验证**：`tests/verify_decode.py` 与 ONNX 逐点比对，平均误差 4.9–6.9px。

---

## 6. `pkill -f` 把自己杀了（调试脚本莫名 exit 137）

**症状**：`adb shell '... pkill -f "python3 -m posturemon" ...'` 返回 137（SIGKILL），
后续命令全不执行，而且目标进程还在。

**根因**：`pgrep/pkill -f` 匹配**整条命令行**，而我自己这条 `bash -c` 的命令行里
就含 `python3 -m posturemon` 这个字符串 → 匹配到自己 → 自杀。
（这也解释了为什么"进程杀不掉"：其实杀的是 shell。）

**修法**：用锚定开头 pattern（`pkill -f "^python3 -m posturemon"`），
或者用 python 遍历 `/proc/*/cmdline` 精确匹配再 `os.kill`。

---

## 7. GTK4 启动报 `MESA: ZINK: vkCreateInstance failed`

**症状**：

```
MESA: error: ZINK: vkCreateInstance failed (VK_ERROR_INCOMPATIBLE_DRIVER)
```

**根因**：Mesa 的 Zink（Vulkan on GL）在 RK3576 上不可用，GTK 探测 Vulkan 失败。

**修法**：启动器里显式 `GSK_RENDERER=ngl`（走 GL 渲染器），错误消失。
非致命，但会拖慢首次绘制。

---

## 8. 板子 RTC 停在 2026-04-13，统计日期全错

**症状**：`ls -l` 时间戳是 4 月，`date` 是 `Mon Apr 13`，写入 DB 的 `day` 字段也是 4 月。

**修法**（部署脚本可加一行）：

```bash
adb shell "date $(date +%m%d%H%M%Y.%S)"     # 用主机时间给板子对时
```

生产环境应配 NTP；本板无外网，只能用主机对时。

---

## 9. 空场景反复误报"有人员/坐姿不良"

**症状**：摄像头前没人，面板时不时变"低头/前倾"，还会累计违规。

**根因**：w8a8 量化模型对椅子/靠垫/海报有 0.45~0.5 的误检，骨架一歪就命中规则。

**修法**（三层）：

1. `conf_threshold` 0.35 → 0.45
2. 几何可信度门 `rules._plausible()`（肩宽下限 / head_drop 范围 / 框长宽比）
3. 缺失 4 帧迟滞（`EngineState.miss`）

实测把空场景误报压到 0。

---

## 10. 排障常用命令

```bash
# 运行态（不用看屏）
adb shell 'cat /run/posture-monitor/status.json'

# 日志
adb shell 'tail -50 /var/log/posture-monitor/app.log'

# 强制 CPU 后端对比
adb shell 'POSTURE_ARGS="--backend=onnx" sudo -u quectel /usr/local/bin/posture-monitor.sh'

# 规则自检（不需要摄像头）
adb shell 'PYTHONPATH=/opt/posture-monitor python3 -m posturemon.headless --selftest-rules'

# 连续指标输出
adb shell 'PYTHONPATH=/opt/posture-monitor python3 -m posturemon.headless --seconds 30'

# 用离线图片跑完整链路
adb shell 'PYTHONPATH=/opt/posture-monitor python3 -m posturemon.headless \
            --source /opt/posture-monitor/samples/zidane.jpg --seconds 5'

# NPU vs ONNX 精度回归
adb shell 'cd /opt/posture-monitor && PYTHONPATH=. python3 tests/verify_decode.py'

# 抓真实屏幕
adb shell 'bash /tmp/posture-src/scripts/screenshot.sh /tmp/s.png' && adb pull /tmp/s.png

# 看 NPU driver / runtime 版本
adb shell 'cat /sys/kernel/debug/rknpu/version'
adb shell 'python3 -c "d=open(\"/usr/lib/librknnrt.so\",\"rb\").read();import re;print(re.search(rb\"librknnrt version: ([0-9.]+)\",d).group(1))"'
```

## 11. 摄像头相关

| 现象 | 原因 | 处理 |
|---|---|---|
| `/dev/video-camera0` 报 `not-negotiated`，`crop-bounds 0,0,800,600` | MIPI CSI 没接摄像头 / media pipeline 未配 | 当前用 USB UVC；MIPI 需 `media-ctl` 按型号配 rkisp 链路 |
| `cv2.VideoCapture(0)` 打不开 | 板端没有 `/dev/video0`，编号到 73+ | 用 `/dev/v4l/by-id/usb-...-video-index0` |
| 首帧全黑 | UVC 启动慢 | `Camera.open()` 里已经预热 3 帧 |
| 只支持 YUYV 640×480 | 摄像头能力限制 | 配置里 `camera_width/height` 就是 640×480 |
