"""V4L2 摄像头采集线程：只保留最新一帧，避免推理跟不上时积压。"""
from __future__ import annotations

import threading
import time

import cv2
import numpy as np


class Camera:
    def __init__(self, device: str, width: int, height: int, fps: int = 25,
                 fourcc: str | None = None):
        self.device = device
        self.width = width
        self.height = height
        self.fps = fps
        self.fourcc = fourcc
        self._cap = None
        self._lock = threading.Lock()
        self._frame = None
        self._seq = 0
        self._stop = threading.Event()
        self._thread = None
        self.error: str | None = None
        self.real_size = (0, 0)

    # ---------------- 生命周期 ----------------
    def open(self) -> None:
        cap = cv2.VideoCapture(self.device, cv2.CAP_V4L2)
        if not cap.isOpened():
            # 允许传索引（如 "0"）走默认后端
            cap = cv2.VideoCapture(int(self.device) if str(self.device).isdigit() else self.device)
        if not cap.isOpened():
            raise RuntimeError(f"打不开摄像头 {self.device}")
        if self.fourcc:
            cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*self.fourcc))
        cap.set(cv2.CAP_PROP_FRAME_WIDTH, self.width)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, self.height)
        cap.set(cv2.CAP_PROP_FPS, self.fps)
        cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
        self._cap = cap
        self.real_size = (int(cap.get(cv2.CAP_PROP_FRAME_WIDTH)),
                          int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT)))
        # 预热，UVC 首帧常常是黑的
        for _ in range(3):
            cap.read()

    def start(self) -> "Camera":
        if self._cap is None:
            self.open()
        self._thread = threading.Thread(target=self._loop, name="camera", daemon=True)
        self._thread.start()
        return self

    def _loop(self):
        fail = 0
        while not self._stop.is_set():
            ok, frame = self._cap.read()
            if not ok or frame is None:
                fail += 1
                self.error = "读帧失败"
                if fail > 50:
                    time.sleep(0.5)
                    fail = 0
                continue
            fail = 0
            self.error = None
            with self._lock:
                self._frame = frame
                self._seq += 1

    # ---------------- 取帧 ----------------
    def read(self, last_seq: int = -1, timeout: float = 2.0):
        """返回 (frame, seq)；seq 未变说明没有新帧。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self._lock:
                if self._frame is not None and self._seq != last_seq:
                    return self._frame.copy(), self._seq
            time.sleep(0.005)
        with self._lock:
            return (None if self._frame is None else self._frame.copy()), self._seq

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2)
        if self._cap is not None:
            self._cap.release()
            self._cap = None


def list_cameras() -> list[str]:
    """列出可用的 V4L2 by-id 设备，优先 UVC。"""
    import glob
    devs = sorted(glob.glob("/dev/v4l/by-id/*video-index0"))
    return [d for d in devs if "usb" in d] + [d for d in devs if "usb" not in d]
