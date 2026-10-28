"""采集 + 推理 + 判定 的流水线。GUI 与 headless 共用。

线程模型：
    camera 线程   —— 只负责从 V4L2 取帧、覆盖式保存最新帧
    infer  线程   —— 取最新帧 -> letterbox -> 推理 -> 选主人物 -> 更新判定状态机
    UI 线程       —— 只读 pipeline.latest()，不阻塞
"""
from __future__ import annotations

import threading
import time

import cv2
import numpy as np

from . import backends, yolopose
from .camera import Camera
from .rules import PostureEngine


class Pipeline:
    def __init__(self, cfg, source: str | None = None, use_camera: bool = True):
        self.cfg = cfg
        self.backend, self.backend_desc = backends.make_backend(cfg)
        self.sdk = getattr(self.backend, "sdk", "?")
        self.engine = PostureEngine(cfg)
        self._lock = threading.Lock()
        self.frame = None
        self.frame_seq = -1
        self.pose: backends.PoseResult | None = None
        self.map_info = None            # (r, pad_x, pad_y)
        self.infer_count = 0
        self.infer_errors = 0
        self.last_error = ""
        self._stop = threading.Event()
        self._thread = None
        self.camera: Camera | None = None
        self._video = None              # 视频文件回放
        self._use_camera = use_camera
        self.source = source

        # 预热：把推理路径里会用到的惰性 import / 算子先跑一遍。
        # numpy 2.x 的 np.ctypeslib 是惰性属性，如果首次访问发生在推理线程，
        # 与采集线程并发 import 会 segfault（实测过一次），所以必须先单线程跑通。
        try:
            self.backend.infer(np.zeros((self.backend.in_h, self.backend.in_w, 3), np.uint8))
        except Exception as e:                     # noqa: BLE001
            self.last_error = f"warmup: {type(e).__name__}: {e}"

        if source:
            self._video = cv2.VideoCapture(source)
            if not self._video.isOpened():
                raise RuntimeError(f"打不开视频源 {source}")
            self.source_fps = self._video.get(cv2.CAP_PROP_FPS) or 25.0
        elif use_camera:
            self.camera = Camera(cfg.camera_device, cfg.camera_width,
                                 cfg.camera_height, cfg.camera_fps)
            self.camera.start()
            self.source_fps = float(cfg.camera_fps)

    # ---------------- 生命周期 ----------------
    def start(self) -> "Pipeline":
        self._thread = threading.Thread(target=self._loop, name="infer", daemon=True)
        self._thread.start()
        return self

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2)
        if self.camera:
            self.camera.stop()
        if self._video is not None:
            self._video.release()
        try:
            self.backend.close()
        except Exception:
            pass

    # ---------------- 取数 ----------------
    def latest(self):
        with self._lock:
            return self.frame, self.frame_seq, self.pose, self.map_info

    # ---------------- 主循环 ----------------
    def _grab(self):
        if self._video is not None:
            ok, frame = self._video.read()
            if not ok:
                self._video.set(cv2.CAP_PROP_POS_FRAMES, 0)   # 循环播放
                ok, frame = self._video.read()
                if not ok:
                    return None, self.frame_seq
            seq = self.frame_seq + 1
            # 按源帧率节流
            time.sleep(1.0 / max(self.source_fps, 1.0))
            return frame, seq
        if self.camera is None:
            return None, self.frame_seq
        return self.camera.read(self.frame_seq, timeout=0.5)

    def _loop(self):
        cfg = self.cfg
        interval = float(cfg.infer_interval_ms) / 1000.0
        last_infer = 0.0
        while not self._stop.is_set():
            frame, seq = self._grab()
            if frame is None or seq == self.frame_seq:
                time.sleep(0.005)
                continue
            now = time.time()
            if now - last_infer < interval:
                with self._lock:                       # 画面照常更新，推理限速
                    self.frame = frame
                    self.frame_seq = seq
                time.sleep(0.004)
                continue
            last_infer = now
            try:
                canvas, r, padx, pady = yolopose.letterbox(frame, self.backend.in_h)
                rgb = cv2.cvtColor(canvas, cv2.COLOR_BGR2RGB)
                pose = self.backend.infer(rgb, cfg.conf_threshold, cfg.iou_threshold)
                self.infer_count += 1
                if len(pose):
                    i = int((pose.boxes[:, 2] * pose.boxes[:, 3]).argmax())
                    self.engine.update(pose.kpts[i], self.backend.in_h,
                                       box=pose.boxes[i], now=now)
                else:
                    self.engine.update(None, self.backend.in_h, now=now)
                with self._lock:
                    self.frame = frame
                    self.frame_seq = seq
                    self.pose = pose
                    self.map_info = (r, padx, pady)
            except Exception as e:                     # noqa: BLE001
                self.infer_errors += 1
                self.last_error = f"{type(e).__name__}: {e}"
                with self._lock:
                    self.frame = frame
                    self.frame_seq = seq
                time.sleep(0.5)

    # ---------------- 诊断 ----------------
    def snapshot(self) -> dict:
        with self._lock:
            pose = self.pose
            st = self.engine.summary()
        out = {
            "backend": self.backend_desc,
            "sdk": self.sdk,
            "model_input": (self.backend.in_h, self.backend.in_w),
            "infer_count": self.infer_count,
            "infer_errors": self.infer_errors,
            "last_error": self.last_error,
            "camera_error": self.camera.error if self.camera else None,
            "camera_size": self.camera.real_size if self.camera else None,
        }
        out.update(st)
        out["latency_ms"] = round(pose.latency_ms, 1) if pose is not None else None
        out["persons"] = len(pose) if pose is not None else 0
        return out
