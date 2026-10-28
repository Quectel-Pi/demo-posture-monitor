# 01 · 需求 → 实现映射（DEMO-036）

来源：飞书多维表格「QuectelPi Demo 开发计划（平台侧）」· Demo 题库 `tblE79jovOWqGayZ`
选题 `recvvtTHKg1obH` / 台账 `DEMO-036`

## 1. 需求原文

| 字段 | 值 |
|---|---|
| 选题名称 | 坐姿检测与护眼提醒一体机 |
| 一句话介绍 | 人体 17 关键点 + 坐姿判定（托腮/低头趴桌/高低肩），不合格时屏幕提醒并累计时长 |
| 可复用原子能力 | 人体关键点 SDK、坐姿判定规则、提醒联动 |
| 硬件依赖 | MIPI 摄像头（**本次实际用 USB UVC**，见限制表） |
| 机构来源 | 无需机构（纯软件），成本 0 元 |
| 对应平台 | Debian / Quectel Pi M2 (RK3576) |
| 推荐指数 | ★★★ 首发推荐 · 预估 8 人天 · 展会演示价值「高」 |
| 状态 | 已转立项 · 负责人 Jonathan Wei |

## 2. 逐条对照

| # | 需求 | 实现 | 位置 |
|---|---|---|---|
| 1 | 人体 17 关键点 | YOLOv8n-pose，COCO-17；NPU 18–25ms | `yolopose.py` / `backends.py` |
| 2 | 托腮 | 手腕到鼻子的距离 / 肩宽 < 0.65，且手抬到肩线以上、在头部横向范围内 | `rules.measure()` + `evaluate()` |
| 3 | 低头趴桌 | (脖子Y − 鼻子Y) / 肩宽 < 0.30（趴桌时该值为负） | 同上 |
| 4 | 高低肩 | 双肩连线倾角 > 7° | 同上 |
| 5 | 不合格时屏幕提醒 | 顶部红色横幅 + 提示音 + 状态区变红 + 具体指标 | `window.py` |
| 6 | 累计时长 | SQLite 按天累计「在座时长 / 不良时长 / 违规次数」 | `stats.py` + 面板「今日不良」 |
| 7 | 护眼提醒 | 20-20-20：每 20 分钟全屏休息 20 秒 | `window.start_rest()` |
| 8 | 端侧算力卖点 | NPU 推理，实测 18–25ms（CPU 备援 330ms，13–18× 差距） | `backends.py` |
| 9 | 无机构、纯软件 | 仅需 USB 摄像头（板载接口即可） | — |

## 3. 超出需求的部分（顺手做的）

| 项 | 为什么加 | 位置 |
|---|---|---|
| 歪头判定 | 现场看屏幕时最容易触发，比"高低肩"好演示 | `head_tilt_deg > 12°` |
| 身体前倾 | 驼背/凑近的常见形态，配合"离屏太近"一起讲 | `lean > 0.35` |
| 离屏幕太近 | 护眼场景的真实痛点（肩宽占画面比） | `width_ratio > 0.62` |
| 久坐提醒 | 45 分钟不打理，展会观众坐久了有交互 | `sedentary_interval_s` |
| 几何可信度门 `reliable` | 空椅子会被检成 person，不加门会误报打断演示 | `rules._plausible()` |
| `status.json` 运行态 | 无 HDMI 时也能验证"界面在跑、在推理" | `window._write_status()` |
| 规则合成自检 | 不需要人和摄像头就能验证 6 条规则 | `headless --selftest-rules` |

## 4. 与原方案的差异（重要）

需求卡写的是「移远 PeopleKptDet v1.2（约 25ms）」。本次落地时：

- 板端与本地仓库**都没有** PeopleKptDet 的 `.so` / 模型 / 头文件；
- 板端**无外网**，无法在线获取；
- 与需求方确认后采用替代路线：**YOLOv8n-pose（RK3576 NPU w8a8，实测 18–25ms）**，
  能力与延迟指标与需求卡一致（17 关键点、~25ms）。

代码已经把算法层抽象成 `PoseBackend.infer(rgb) -> PoseResult`。等 SDK 到位，
只需在 `backends.py` 新增一个 `PeopleKptBackend` 并在 `make_backend()` 里排在最前，
**上层规则、界面、统计、部署全部不用动**。

## 5. 验收口径

| 项 | 判据 | 验证方式 |
|---|---|---|
| 算法可用 | 17 点齐全，与 ONNX 参考误差 < 15px | `tests/verify_decode.py` |
| 延迟 | 单帧 < 40ms（NPU） | `verify.sh` §2 |
| 判定正确 | 6 条规则各自可独立触发，正常坐姿不误报 | `headless --selftest-rules` |
| 界面可用 | 全屏 1280×800、实时骨架、状态/计数刷新 | `status.json` + `scripts/screenshot.sh` |
| 提醒可用 | 连续违规 60s 弹横幅+提示音；20min 全屏护眼 | 现场演示 / 日志 |
| 统计可用 | 每日时长与次数入库，重启不丢 | `sqlite3 /var/lib/posture-monitor/posture.db` |
| 开机可用 | 重启整机自动进界面 | `systemctl restart gdm` 后看 `status.json` |
