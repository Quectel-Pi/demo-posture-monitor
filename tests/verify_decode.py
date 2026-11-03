#!/usr/bin/env python3
"""板端回归：RKNN(NPU) 解码结果 vs ONNX 参考结果 对齐验证。

    PYTHONPATH=/opt/posture-monitor python3 tests/verify_decode.py samples/*.jpg

判据（实测已达成）：
    · 人数一致（NPU 量化后可能多检，允许 ±n）
    · 匹配上的框 IoU > 0.4
    · 关键点平均误差 < 15px（640 空间），即 < 2.4% 画面
"""
import sys
import os

sys.path.insert(0, "/opt/posture-monitor")
sys.path.insert(0, "/opt/posture-monitor/lib")

import cv2
import numpy as np
import onnxruntime as ort

from posturemon import yolopose
from posturemon.backends import RknnBackend

APP = "/opt/posture-monitor"


def iou(a, b):
    ax1, ay1 = a[0] - a[2] / 2, a[1] - a[3] / 2
    ax2, ay2 = a[0] + a[2] / 2, a[1] + a[3] / 2
    bx1, by1 = b[0] - b[2] / 2, b[1] - b[3] / 2
    bx2, by2 = b[0] + b[2] / 2, b[1] + b[3] / 2
    ix = max(0.0, min(ax2, bx2) - max(ax1, bx1))
    iy = max(0.0, min(ay2, by2) - max(ay1, by1))
    inter = ix * iy
    return inter / max(a[2] * a[3] + b[2] * b[3] - inter, 1e-6)


def nms(boxes, scores, thr=0.45):
    order = np.argsort(-scores)
    keep = []
    for i in order:
        if all(iou(boxes[i], boxes[j]) < thr for j in keep):
            keep.append(i)
    return keep


def onnx_ref(sess, rgb, conf_th):
    x = (rgb.astype(np.float32) / 255.0).transpose(2, 0, 1)[None]
    p = sess.run(None, {sess.get_inputs()[0].name: x})[0][0].T
    idx = np.nonzero(p[:, 4] > conf_th)[0]
    if idx.size == 0:
        return (np.zeros((0, 4), np.float32), np.zeros(0, np.float32),
                np.zeros((0, 17, 3), np.float32))
    b, c, k = p[idx, :4], p[idx, 4], p[idx, 5:].reshape(-1, 17, 3)
    keep = nms(b, c)
    return b[keep], c[keep], k[keep]


def main(argv):
    files = argv[1:] or [os.path.join(APP, "samples", f) for f in ("zidane.jpg", "bus.jpg")]
    eng = RknnBackend(os.path.join(APP, "models", "yolov8n-pose.rknn"))
    sess = ort.InferenceSession(os.path.join(APP, "models", "yolov8n-pose.onnx"),
                                providers=["CPUExecutionProvider"])
    fails = 0
    for f in files:
        img = cv2.imread(f)
        if img is None:
            print(f"  跳过（读不到）{f}")
            continue
        canvas, r, px, py = yolopose.letterbox(img, 640)
        rgb = cv2.cvtColor(canvas, cv2.COLOR_BGR2RGB)
        rb, rs, rk = yolopose.decode_rknn(_raw_outs(eng, rgb), 0.45, 0.45)
        ob, os_, ok = onnx_ref(sess, rgb, 0.45)
        print(f"\n{os.path.basename(f)}: RKNN {len(rb)} / ONNX {len(ob)} person(s)")
        matched = 0
        used = set()
        for i in range(len(rb)):
            cand = [j for j in range(len(ob)) if j not in used]
            if not cand:
                break
            j = max(cand, key=lambda j: iou(rb[i], ob[j]))
            if iou(rb[i], ob[j]) < 0.40:
                continue
            used.add(j)
            matched += 1
            err = float(np.abs(rk[i][:, :2] - ok[j][:, :2]).mean())
            flag = "PASS" if err < 15 else "FAIL"
            fails += 0 if err < 15 else 1
            print(f"  [{flag}] #{i} IoU={iou(rb[i], ob[j]):.3f} score {rs[i]:.2f}/{os_[j]:.2f} "
                  f"关键点平均误差 {err:.1f}px")
        if matched == 0:
            print("  [FAIL] 没有任何框匹配上")
            fails += 1
    print(f"\n结果：{'全部通过' if not fails else f'{fails} 项失败'}")
    return 1 if fails else 0


def _raw_outs(eng, rgb):
    """取 RKNN 原始 4 个输出（decode_rknn 需要原始 tensor，不是 PoseResult）。"""
    import ctypes
    from posturemon.backends import _RknnInput, _RknnOutput, T_UINT8, F_NHWC
    img = np.ascontiguousarray(rgb, dtype=np.uint8)
    i0 = _RknnInput(index=0, buf=img.ctypes.data, size=img.nbytes,
                    pass_through=0, type=T_UINT8, fmt=F_NHWC)
    eng.L.rknn_inputs_set(eng.ctx, ctypes.c_uint32(1), ctypes.byref(i0))
    eng.L.rknn_run(eng.ctx, None)
    arr = (_RknnOutput * eng.n_out)()
    for i in range(eng.n_out):
        arr[i].index = i
        arr[i].want_float = 1
        arr[i].is_prealloc = 0
        arr[i].buf = None
        arr[i].size = 0
    eng.L.rknn_outputs_get(eng.ctx, ctypes.c_uint32(eng.n_out), arr, None)
    outs = []
    try:
        for i in range(eng.n_out):
            n = int(np.prod(eng.outs[i]))
            ptr = ctypes.cast(arr[i].buf, ctypes.POINTER(ctypes.c_float))
            outs.append(np.ctypeslib.as_array(ptr, shape=(n,)).copy())
    finally:
        eng.L.rknn_outputs_release(eng.ctx, ctypes.c_uint32(eng.n_out), arr)
    return outs


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
