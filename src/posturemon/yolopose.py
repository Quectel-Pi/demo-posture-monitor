"""YOLOv8-pose 后处理：RKNN split-head（NCHW）与 ONNX 单输出 (1,56,8400) 两种格式。

RKNN 模型（RKNNAI/RK3576-CNN-yolov8n-pose, w8a8）输出 4 个 tensor：
    (1,65,80,80) (1,65,40,40) (1,65,20,20)  -> 每个 grid: 64 ch DFL + 1 ch conf(不 sigmoid)
    (1,17,3,8400)                            -> 17 个关键点 x,y,conf（按 anchor 排列）

ONNX 模型（Xenova/yolov8n-pose, fp32）输出 1 个 tensor：
    (1,56,8400) -> cx,cy,w,h,conf, 17*3 关键点

两者都是 640x640 letterbox 输入、坐标单位为 640 输入空间。
"""
from __future__ import annotations

import numpy as np

IMG_SIZE = 640
STRIDES = (8, 16, 32)
GRID = (80, 40, 20)
ANCHOR_OFFSET = (0, 6400, 8000)
KPT_N = 17
KPT_NAMES = ("nose", "eye_l", "eye_r", "ear_l", "ear_r", "shoulder_l", "shoulder_r",
             "elbow_l", "elbow_r", "wrist_l", "wrist_r", "hip_l", "hip_r",
             "knee_l", "knee_r", "ankle_l", "ankle_r")
# COCO-17 骨架（1-based，与 ultralytics 一致）
SKELETON = ((16, 14), (14, 12), (17, 15), (15, 13), (12, 13), (6, 12), (7, 13),
            (6, 7), (6, 8), (7, 9), (8, 10), (9, 11), (2, 3), (1, 2), (1, 3),
            (2, 4), (3, 5), (4, 6), (5, 7))


def sigmoid(x):
    return 1.0 / (1.0 + np.exp(-np.clip(x, -60.0, 60.0)))


def _dfl(t):
    """t: (n,4,16) -> (n,4) 期望值"""
    e = np.exp(t - t.max(axis=-1, keepdims=True))
    p = e / e.sum(axis=-1, keepdims=True)
    return (p * np.arange(16, dtype=np.float32)).sum(axis=-1)


def _nms(boxes, scores, thr):
    x1 = boxes[:, 0] - boxes[:, 2] / 2
    y1 = boxes[:, 1] - boxes[:, 3] / 2
    x2 = boxes[:, 0] + boxes[:, 2] / 2
    y2 = boxes[:, 1] + boxes[:, 3] / 2
    order = scores.argsort()[::-1]
    keep = []
    while order.size:
        i = order[0]
        keep.append(int(i))
        if order.size == 1:
            break
        rest = order[1:]
        ix = np.maximum(0.0, np.minimum(x2[i], x2[rest]) - np.maximum(x1[i], x1[rest]))
        iy = np.maximum(0.0, np.minimum(y2[i], y2[rest]) - np.maximum(y1[i], y1[rest]))
        inter = ix * iy
        union = ((x2[i] - x1[i]) * (y2[i] - y1[i])
                 + (x2[rest] - x1[rest]) * (y2[rest] - y1[rest]) - inter)
        order = rest[(inter / np.maximum(union, 1e-6)) <= thr]
    return keep


def decode_rknn(outs, conf_th=0.35, iou_th=0.45):
    """RKNN 4 输出 -> boxes(N,4 xywh), scores(N,), kpts(N,17,3)（640 空间）"""
    acc = []
    for si, (gs, st) in enumerate(zip(GRID, STRIDES)):
        t = outs[si].reshape(65, gs, gs).astype(np.float32)      # NCHW
        conf = sigmoid(t[64])
        ys, xs = np.nonzero(conf > conf_th)
        if ys.size == 0:
            continue
        d = _dfl(np.transpose(t[:64, ys, xs], (1, 0)).reshape(-1, 4, 16))
        acc.append((
            (xs + 0.5 - d[:, 0] + d[:, 2]) * st,
            (ys + 0.5 - d[:, 1] + d[:, 3]) * st,
            (d[:, 0] + d[:, 2]) * st,
            (d[:, 1] + d[:, 3]) * st,
            conf[ys, xs],
            ys * gs + xs + ANCHOR_OFFSET[si],
        ))
    if not acc:
        return _empty()
    cx, cy, bw, bh, sc, flat = (np.concatenate([a[i] for a in acc]) for i in range(6))
    boxes = np.stack([cx, cy, bw, bh], 1).astype(np.float32)
    keep = _nms(boxes, sc, iou_th)
    boxes, sc, flat = boxes[keep], sc[keep], flat[keep].astype(np.int64)
    kk = outs[3].reshape(KPT_N, 3, 8400).astype(np.float32)
    kpts = np.transpose(kk[:, :, flat], (2, 0, 1)).copy()
    return boxes, sc, kpts


def decode_onnx(out, conf_th=0.35, iou_th=0.45):
    """ONNX (1,56,8400) -> 同上"""
    p = np.asarray(out, dtype=np.float32).reshape(56, 8400).T      # (8400,56)
    idx = np.nonzero(p[:, 4] > conf_th)[0]
    if idx.size == 0:
        return _empty()
    boxes = p[idx, :4].copy()
    scores = p[idx, 4].copy()
    kpts = p[idx, 5:].reshape(-1, KPT_N, 3).copy()
    keep = _nms(boxes, scores, iou_th)
    return boxes[keep], scores[keep], kpts[keep]


def _empty():
    return (np.zeros((0, 4), np.float32), np.zeros(0, np.float32),
            np.zeros((0, KPT_N, 3), np.float32))


def letterbox(img, size=IMG_SIZE, color=114):
    """等比例缩放 + 灰边填充。返回 (canvas, ratio, pad_x, pad_y)"""
    import cv2
    h, w = img.shape[:2]
    r = min(size / h, size / w)
    nw, nh = int(round(w * r)), int(round(h * r))
    dw, dh = (size - nw) // 2, (size - nh) // 2
    canvas = np.full((size, size, 3), color, np.uint8)
    canvas[dh:dh + nh, dw:dw + nw] = cv2.resize(img, (nw, nh), interpolation=cv2.INTER_LINEAR)
    return canvas, r, dw, dh
