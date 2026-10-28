"""Cairo 绘制：视频帧 + 关键点骨架 + 告警横幅。"""
from __future__ import annotations

import cairo
import numpy as np

from . import yolopose

# 骨架分色：躯干/左/右
COLOR_LEFT = (0.29, 0.72, 1.00)
COLOR_RIGHT = (1.00, 0.72, 0.25)
COLOR_TORSO = (0.45, 0.90, 0.55)
COLOR_OK = (0.30, 0.85, 0.45)
COLOR_BAD = (1.00, 0.33, 0.33)

LEFT_IDX = {1, 3, 5, 7, 9, 11, 13, 15}
TORSO_LINKS = {(12, 13), (6, 7), (6, 12), (7, 13), (6, 7)}


class FrameSurface:
    """把 BGR ndarray 包成 cairo surface。

    ★ 不要复用同一个 ImageSurface + mark_dirty()：GTK4 的 DrawingArea 绘制会在
      recording/recording surface 上保留快照，之后再改同一块像素会直接 abort：
      `cairo_surface_mark_dirty_rectangle: Assertion !_cairo_surface_has_snapshots`
      （踩过一次，表现为程序莫名 Aborted、没有 traceback）。
      所以每帧用环形缓冲里的一块**新** buffer + 新建 surface，不调 mark_dirty。
    """

    RING = 4

    def __init__(self):
        self._bufs: list[np.ndarray] = []
        self._idx = 0
        self._shape = None
        self._keepalive: list = []      # 保留最近几帧，防止 GTK 还引用时被回收

    def update(self, bgr: np.ndarray) -> cairo.ImageSurface:
        import cv2
        h, w = bgr.shape[:2]
        if self._shape != (h, w) or not self._bufs:
            self._bufs = [np.empty((h, w, 4), np.uint8) for _ in range(self.RING)]
            self._shape = (h, w)
            self._idx = 0
            self._keepalive = []
        buf = self._bufs[self._idx]
        self._idx = (self._idx + 1) % self.RING
        # FORMAT_RGB24 在小端下按 B,G,R,X 解释 —— 与 cv2 的 BGRA 恰好一致
        cv2.cvtColor(bgr, cv2.COLOR_BGR2BGRA, dst=buf)
        surf = cairo.ImageSurface.create_for_data(buf, cairo.FORMAT_RGB24, w, h, w * 4)
        self._keepalive.append((buf, surf))
        if len(self._keepalive) > self.RING:
            self._keepalive.pop(0)
        return surf


def _skeleton_color(a: int, b: int):
    """输入 1-based 骨架端点。"""
    pair = tuple(sorted((a, b)))
    if pair in TORSO_LINKS:
        return COLOR_TORSO
    return COLOR_LEFT if (a in LEFT_IDX or b in LEFT_IDX) else COLOR_RIGHT


def draw_skeleton(ctx, kpts, to_widget, bad=False, line_w=2.5, show_names=False):
    """kpts: (17,3) 模型空间；to_widget(x, y) -> (wx, wy)"""
    if kpts is None or len(kpts) < 17:
        return
    pts = [to_widget(float(kpts[i][0]), float(kpts[i][1])) for i in range(17)]

    ctx.save()
    ctx.set_line_cap(cairo.LINE_CAP_ROUND)
    for a, b in yolopose.SKELETON:
        color = COLOR_BAD if bad else _skeleton_color(a, b)
        ctx.set_source_rgba(*color, 0.95)
        ctx.set_line_width(line_w)
        ctx.move_to(*pts[a - 1])
        ctx.line_to(*pts[b - 1])
        ctx.stroke()
    for i, p in enumerate(pts):
        color = COLOR_BAD if bad else (_skeleton_color(i + 1, i + 1)
                                       if i + 1 not in (1, 2, 3, 4) else COLOR_TORSO)
        ctx.set_source_rgba(*color, 1.0)
        ctx.arc(p[0], p[1], 3.2, 0, 2 * 3.141592653589793)
        ctx.fill()
    if show_names:
        ctx.select_font_face("Sans", cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_NORMAL)
        ctx.set_font_size(11)
        for i, p in enumerate(pts):
            ctx.set_source_rgba(1, 1, 1, 0.65)
            ctx.move_to(p[0] + 4, p[1] - 4)
            ctx.show_text(yolopose.KPT_NAMES[i])
    ctx.restore()


def draw_box(ctx, box, to_widget, color=(0.30, 0.85, 0.45), label=""):
    x, y, w, h = box
    x1, y1 = to_widget(x - w / 2, y - h / 2)
    x2, y2 = to_widget(x + w / 2, y + h / 2)
    ctx.save()
    ctx.set_source_rgba(*color, 0.9)
    ctx.set_line_width(2)
    ctx.rectangle(x1, y1, max(1, x2 - x1), max(1, y2 - y1))
    ctx.stroke()
    if label:
        ctx.select_font_face("Sans", cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_BOLD)
        ctx.set_font_size(13)
        ext = ctx.text_extents(label)
        ctx.set_source_rgba(0, 0, 0, 0.55)
        ctx.rectangle(x1, y1 - 20, ext.width + 12, 20)
        ctx.fill()
        ctx.set_source_rgba(*color, 1)
        ctx.move_to(x1 + 6, y1 - 6)
        ctx.show_text(label)
    ctx.restore()
