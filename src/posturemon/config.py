"""运行参数。默认值与判定阈值集中在这里，可用 /etc/posture-monitor/config.json 覆盖。"""
from __future__ import annotations

import json
import os
from dataclasses import dataclass, field, asdict

CONFIG_PATHS = (
    "/etc/posture-monitor/config.json",
    os.path.expanduser("~/.config/posture-monitor/config.json"),
)

DEFAULTS = {
    # ---- 采集 ----
    "camera_device": "/dev/video73",
    "camera_width": 640,
    "camera_height": 480,
    "camera_fps": 25,
    "mirror": True,                 # 面向用户的镜子效果（matte 屏常见）
    # ---- 推理 ----
    "model_rknn": "/opt/posture-monitor/models/yolov8n-pose-rknn.rknn",
    "model_onnx": "/opt/posture-monitor/models/yolov8n-pose.onnx",
    "backend": "rknn",              # rknn | onnx | auto
    "infer_interval_ms": 80,        # 推理最小间隔，避免空转吃满 CPU/NPU
    "conf_threshold": 0.45,
    "iou_threshold": 0.45,
    # ---- 关键点平滑 ----
    "smooth_alpha": 0.5,            # EMA 系数，越大越跟手、越小越稳
    "kpt_min_score": 0.35,          # ONNX 后端有效；RKNN 量化后该通道不可靠，见 rules
    # ---- 坐姿判定阈值（单位：肩宽 sw / 像素）----
    "head_drop_bad": 0.30,          # (neck.y-nose.y)/sw 低于此值 = 低头/趴桌
    "head_drop_warn": 0.45,
    "shoulder_tilt_deg": 7.0,       # 双肩连线倾角超过 = 高低肩
    "head_tilt_deg": 12.0,          # 双眼连线倾角超过 = 歪头
    "hand_to_face_ratio": 0.65,     # 手腕-鼻子距离 / sw 小于 = 托腮
    "lean_ratio": 0.35,             # |nose.x-neck.x| / sw 超过 = 身体前倾
    "too_close_ratio": 0.62,        # sw / 画面宽 超过 = 离屏幕太近
    # ---- 状态机 ----
    "violation_hold_s": 2.0,        # 违规需持续多久才计数
    "violation_clear_s": 1.5,       # 恢复正常多久后清除
    "bad_posture_remind_s": 60.0,   # 连续不良多久弹提醒
    "remind_cooldown_s": 90.0,      # 两次提醒最小间隔
    "eye_rest_interval_s": 1200.0,  # 20 分钟（20-20-20 护眼）
    "eye_rest_duration_s": 20.0,
    "sedentary_interval_s": 2700.0, # 45 分钟久坐提醒
    # ---- 统计 ----
    "db_path": "/var/lib/posture-monitor/posture.db",
    "log_dir": "/var/log/posture-monitor",
    # ---- UI ----
    "fullscreen": True,
    "show_names": True,
}


@dataclass
class Config:
    values: dict = field(default_factory=lambda: dict(DEFAULTS))

    @classmethod
    def load(cls, extra: dict | None = None) -> "Config":
        v = dict(DEFAULTS)
        for p in CONFIG_PATHS:
            try:
                with open(p, encoding="utf-8") as f:
                    v.update(json.load(f))
            except FileNotFoundError:
                continue
            except Exception:
                pass
        if extra:
            v.update(extra)
        return cls(values=v)

    def __getattr__(self, item):
        try:
            return self.values[item]
        except KeyError as e:
            raise AttributeError(item) from e

    def __setitem__(self, k, val):
        self.values[k] = val

    def to_dict(self) -> dict:
        return dict(self.values)

    def dump(self, path: str) -> None:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(asdict(self)["values"], f, ensure_ascii=False, indent=2)
