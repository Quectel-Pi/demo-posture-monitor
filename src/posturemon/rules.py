"""坐姿判定：从 17 关键点算几何指标，再用带迟滞的状态机判定违规。

DEMO-036 要求的三类：
    · 托腮（下巴/脸颊靠手）
    · 低头趴桌（含低头看书、趴桌）
    · 高低肩
本实现另外补了 4 类（歪头、贴近屏幕、身体前倾、久坐），因为现场演示时这几种
比"高低肩"更容易被触发、也更好讲。

所有阈值都是**相对量**（以肩宽 sw 为单位），所以不需要标定相机距离。
"""
from __future__ import annotations

import math
import time
from dataclasses import dataclass, field

import numpy as np

# COCO-17 下标
NOSE = 0
EYE_L, EYE_R = 1, 2
EAR_L, EAR_R = 3, 4
SH_L, SH_R = 5, 6
EL_L, EL_R = 7, 8
WR_L, WR_R = 9, 10
HIP_L, HIP_R = 11, 12

VIOLATION_LABEL = {
    "head_down": "低头/趴桌",
    "hand_on_face": "托腮",
    "uneven_shoulder": "高低肩",
    "head_tilt": "歪头",
    "too_close": "离屏幕太近",
    "lean_forward": "身体前倾",
}
# 优先级：越靠前越"严重"，同帧多个违规时只报第一个
VIOLATION_PRIORITY = ("head_down", "hand_on_face", "lean_forward",
                      "uneven_shoulder", "too_close", "head_tilt")


def _pt(k, i):
    return float(k[i][0]), float(k[i][1])


def _valid(k, ids):
    return all(k[i][2] > 0.0 or True for i in ids)


@dataclass
class Metrics:
    shoulder_width: float = 0.0
    head_drop: float = 99.0
    shoulder_tilt_deg: float = 0.0
    head_tilt_deg: float = 0.0
    hand_to_face: float = 99.0
    lean: float = 0.0
    width_ratio: float = 0.0
    box_area: float = 0.0
    ok: bool = False
    reliable: bool = False        # 关键点几何是否可信（挡掉误检/畸形骨架）

    def as_dict(self):
        return {k: (round(float(v), 3) if isinstance(v, (int, float)) else v)
                for k, v in self.__dict__.items() if k != "ok"}


def measure(kpts, frame_w: float, box=None) -> Metrics:
    """kpts: (17,3)，640 输入空间里的坐标（已换算回原图亦可用，只要统一）。"""
    m = Metrics()
    try:
        shl = np.array(_pt(kpts, SH_L))
        shr = np.array(_pt(kpts, SH_R))
        nose = np.array(_pt(kpts, NOSE))
    except Exception:
        return m
    sw = float(np.linalg.norm(shl - shr))
    if sw < 12.0:                      # 肩宽太小 = 人太远/检测不可靠
        return m
    neck = (shl + shr) / 2.0
    m.shoulder_width = sw
    m.head_drop = float((neck[1] - nose[1]) / sw)
    m.shoulder_tilt_deg = float(math.degrees(math.atan2(abs(shl[1] - shr[1]),
                                                        max(abs(shl[0] - shr[0]), 1e-3))))
    # 歪头：优先双眼，退化到双耳
    if kpts[EYE_L][2] > 0 and kpts[EYE_R][2] > 0:
        a, b = _pt(kpts, EYE_L), _pt(kpts, EYE_R)
    else:
        a, b = _pt(kpts, EAR_L), _pt(kpts, EAR_R)
    m.head_tilt_deg = float(math.degrees(math.atan2(abs(a[1] - b[1]), max(abs(a[0] - b[0]), 1e-3))))
    # 托腮：手腕靠近脸、且抬到肩线以上、且在头的横向范围内（排除手放桌面）
    for w in (WR_L, WR_R):
        wr = np.array(_pt(kpts, w))
        if wr[1] > neck[1] + 0.10 * sw:          # 手没抬起来
            continue
        if abs(wr[0] - nose[0]) > 0.90 * sw:     # 手在身体侧面，不是托腮
            continue
        m.hand_to_face = min(m.hand_to_face, float(np.linalg.norm(wr - nose)) / sw)
    # 前倾：优先用髋部中线，髋不可见时退化为鼻子相对脖子
    if kpts[HIP_L][2] >= 0 and kpts[HIP_R][2] >= 0:
        hip = (np.array(_pt(kpts, HIP_L)) + np.array(_pt(kpts, HIP_R))) / 2.0
        torso = max(float(np.linalg.norm(neck - hip)), 1e-3)
        m.lean = float(abs(nose[0] - hip[0]) / torso)
    else:
        m.lean = float(abs(nose[0] - neck[0]) / sw)
    m.width_ratio = float(sw / max(frame_w, 1.0))
    if box is not None:
        m.box_area = float(box[2] * box[3])
    m.ok = True
    m.reliable = _plausible(m, box, frame_w)
    return m


def _plausible(m: "Metrics", box, frame_w: float) -> bool:
    """挡掉误检：空的椅子/海报/靠垫也会被检成 person，骨架一歪就误报提醒。

    判定条件都很宽松，只是排除"明显不是坐着的人"的情形。
    """
    if not m.ok:
        return False
    if m.shoulder_width < 0.05 * frame_w:       # 人太远 / 关键点散架
        return False
    if not (-0.80 <= m.head_drop <= 1.80):      # 鼻子跑到肩膀以下或远高于头
        return False
    if box is not None:
        bw, bh = float(box[2]), float(box[3])
        if bh < 0.20 * frame_w:                 # 目标太小
            return False
        if bw <= 1 or not (0.8 <= bh / bw <= 5.0):   # 长宽比不像人
            return False
    return True


def evaluate(m: Metrics, cfg) -> list[str]:
    """返回本帧命中的违规 key 列表。"""
    if not m.ok:
        return []
    hits = []
    if m.head_drop < cfg.head_drop_bad:
        hits.append("head_down")
    if m.hand_to_face < cfg.hand_to_face_ratio:
        hits.append("hand_on_face")
    if m.lean > cfg.lean_ratio:
        hits.append("lean_forward")
    if m.shoulder_tilt_deg > cfg.shoulder_tilt_deg:
        hits.append("uneven_shoulder")
    if m.width_ratio > cfg.too_close_ratio:
        hits.append("too_close")
    if m.head_tilt_deg > cfg.head_tilt_deg:
        hits.append("head_tilt")
    return hits


@dataclass
class RuleState:
    raw_on: bool = False           # 原始命中（未迟滞）
    on: bool = False               # 迟滞后的稳定状态
    since: float = 0.0             # 本次违规开始时间
    last_on: float = 0.0           # 最近一次命中时间
    count: int = 0                 # 今日次数


@dataclass
class EngineState:
    present: bool = False
    present_since: float = 0.0
    absent_since: float = 0.0
    bad: bool = False
    bad_since: float = 0.0
    last_bad: float = 0.0
    main: str = ""                 # 当前主违规 key
    active: list = field(default_factory=list)
    metrics: Metrics = field(default_factory=Metrics)
    smoothed: np.ndarray | None = None
    miss: int = 0                 # 连续"没看到可信人"的推理帧数


class PostureEngine:
    def __init__(self, cfg, now: float | None = None):
        self.cfg = cfg
        self.now = now if now is not None else time.time()
        self.rules = {k: RuleState() for k in VIOLATION_LABEL}
        self.st = EngineState()
        self._last_kpts = None
        self.total_bad_s = 0.0         # 本次会话累计不良时长
        self._last_tick = self.now
        # 违规事件回调：(key, ts) -> None。用于把每一次违规落库（不依赖是否触发提醒）
        self.on_violation_start = None
        self.on_violation_end = None

    def _emit_start(self, key, ts):
        cb = self.on_violation_start
        if cb:
            try:
                cb(key, ts)
            except Exception:
                pass

    def _emit_end(self, key, ts):
        cb = self.on_violation_end
        if cb:
            try:
                cb(key, ts)
            except Exception:
                pass

    # ---------------- 关键点平滑 ----------------
    def _smooth(self, kpts):
        a = float(self.cfg.smooth_alpha)
        if self._last_kpts is None or self._last_kpts.shape != kpts.shape:
            self._last_kpts = kpts.copy()
        else:
            self._last_kpts = a * kpts + (1 - a) * self._last_kpts
        out = self._last_kpts.copy()
        out[:, 2] = kpts[:, 2]                   # 置信度不做平滑
        return out

    def reset_person(self):
        self._last_kpts = None

    # ---------------- 主循环 ----------------
    def update(self, kpts, frame_w: float, box=None, now: float | None = None) -> EngineState:
        now = now if now is not None else time.time()
        dt = max(0.0, now - self._last_tick)
        self._last_tick = now
        st = self.st

        if kpts is None:
            st.miss += 1
            if st.miss < 4:          # 迟滞：约 0.3~0.5s 内的丢帧不算人走了
                return st
            st.present = False
            if st.present_since and not st.absent_since:
                st.absent_since = now
            if st.bad:
                self.total_bad_s += dt
            st.bad = False
            st.main = ""
            st.active = []
            self.reset_person()
            for r in self.rules.values():
                r.raw_on = False
                if r.on and now - r.last_on > self.cfg.violation_clear_s:
                    r.on = False
            return st

        st.miss = 0
        if not st.present:
            st.present = True
            st.present_since = now
            st.absent_since = 0.0

        k = self._smooth(np.asarray(kpts, dtype=np.float32))
        m = measure(k, frame_w, box)
        st.metrics = m
        st.smoothed = k
        if not m.reliable:
            # 骨架不可信时按"无人"处理，避免误报打断演示（同样走迟滞）
            st.miss += 1
            if st.miss < 4:
                return st
            st.present = False
            st.bad = False
            st.main = ""
            st.active = []
            self.reset_person()
            for r in self.rules.values():
                r.raw_on = False
                r.on = False
                r.since = 0.0
            return st
        hits = evaluate(m, self.cfg)

        # 迟滞状态机：
        #   命中 -> 记录本轮连续命中的起点；持续 >= hold 才置 on 并计数
        #   未命中 -> 保持 on 直到距离最后一次命中超过 clear 才复位
        for key, r in self.rules.items():
            hit = key in hits
            r.raw_on = hit
            if hit:
                r.last_on = now
                if not r.on:
                    if r.since == 0.0:
                        r.since = now
                    if now - r.since >= self.cfg.violation_hold_s:
                        r.on = True
                        r.count += 1
                        self._emit_start(key, r.since)
            else:
                if r.on and now - r.last_on > self.cfg.violation_clear_s:
                    r.on = False
                    self._emit_end(key, r.last_on)
                r.since = 0.0

        active = [key for key in VIOLATION_PRIORITY if self.rules[key].on]
        st.active = active
        was_bad = st.bad
        st.bad = bool(active)
        if st.bad:
            st.last_bad = now
            if not was_bad:
                st.bad_since = now
            self.total_bad_s += dt
        else:
            st.bad_since = 0.0
        st.main = active[0] if active else ""
        return st

    # ---------------- 派生量 ----------------
    def bad_duration(self, now: float | None = None) -> float:
        now = now if now is not None else time.time()
        if self.st.bad and self.st.bad_since:
            return now - self.st.bad_since
        return 0.0

    def main_label(self) -> str:
        return VIOLATION_LABEL.get(self.st.main, "")

    def counts(self) -> dict:
        return {k: r.count for k, r in self.rules.items() if r.count}

    def summary(self) -> dict:
        m = self.st.metrics
        return {
            "present": self.st.present,
            "bad": self.st.bad,
            "main": self.st.main,
            "label": self.main_label(),
            "bad_duration_s": round(self.bad_duration(), 1),
            "total_bad_s": round(self.total_bad_s, 1),
            "metrics": m.as_dict(),
            "counts": self.counts(),
        }
