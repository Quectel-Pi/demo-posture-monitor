#!/usr/bin/env python3
"""M2 验收：把 C++ 引擎的关键点输出与 ONNX fp32 参考模型逐点比对

用法（在**主机**上跑，需要 onnxruntime + opencv）：
    adb shell '/opt/posture-guard/bin/posture_guard --config /etc/posture-guard/posture.yaml \
        --image /path/bus.jpg --dump-keypoints /tmp/bus_cpp.json'
    adb pull /tmp/bus_cpp.json .
    python3 tests/compare_onnx.py samples/bus.jpg bus_cpp.json

判据（对应实施方案 §10.1 第 5 步「误差可解释后才接摄像头」）：
    * 匹配上的人体关键点平均误差 <= 25px（w8a8 量化模型的经验上限）
    * 检出人数不超过参考值的 3 倍（conf 通道量化粗，重复框是已知现象）
    * 匹配率 >= 0.5
"""
from __future__ import annotations

import json
import sys

import numpy as np

IMG = 640
CONF_TH = 0.45


def letterbox(img, size=IMG, color=114):
    import cv2

    h, w = img.shape[:2]
    r = min(size / h, size / w)
    nw, nh = int(round(w * r)), int(round(h * r))
    dw, dh = (size - nw) // 2, (size - nh) // 2
    canvas = np.full((size, size, 3), color, np.uint8)
    canvas[dh:dh + nh, dw:dw + nw] = cv2.resize(img, (nw, nh))
    return canvas, r, dw, dh


def nms(boxes, scores, thr=0.45):
    x1 = boxes[:, 0] - boxes[:, 2] / 2
    y1 = boxes[:, 1] - boxes[:, 3] / 2
    x2 = boxes[:, 0] + boxes[:, 2] / 2
    y2 = boxes[:, 1] + boxes[:, 3] / 2
    order = scores.argsort()[::-1]
    keep = []
    while order.size:
        i = order[0]
        keep.append(int(i))
        rest = order[1:]
        ix = np.maximum(0.0, np.minimum(x2[i], x2[rest]) - np.maximum(x1[i], x1[rest]))
        iy = np.maximum(0.0, np.minimum(y2[i], y2[rest]) - np.maximum(y1[i], y1[rest]))
        inter = ix * iy
        union = ((x2[i] - x1[i]) * (y2[i] - y1[i])
                 + (x2[rest] - x1[rest]) * (y2[rest] - y1[rest]) - inter)
        order = rest[(inter / np.maximum(union, 1e-6)) <= thr]
    return keep


def onnx_reference(model_path: str, bgr) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    import onnxruntime as ort

    canvas, r, dw, dh = letterbox(bgr)
    import cv2

    x = (cv2.cvtColor(canvas, cv2.COLOR_BGR2RGB).astype(np.float32) / 255.0)
    x = x.transpose(2, 0, 1)[None]
    sess = ort.InferenceSession(model_path, providers=["CPUExecutionProvider"])
    out = sess.run(None, {sess.get_inputs()[0].name: x})[0].reshape(56, 8400).T
    idx = np.nonzero(out[:, 4] > CONF_TH)[0]
    boxes = out[idx, :4]
    scores = out[idx, 4]
    kpts = out[idx, 5:].reshape(-1, 17, 3)
    keep = nms(boxes, scores)
    boxes, scores, kpts = boxes[keep], scores[keep], kpts[keep]
    # 模型空间 -> 原图
    boxes = np.stack([(boxes[:, 0] - dw) / r, (boxes[:, 1] - dh) / r,
                      boxes[:, 2] / r, boxes[:, 3] / r], 1)
    kpts = kpts.copy()
    kpts[:, :, 0] = (kpts[:, :, 0] - dw) / r
    kpts[:, :, 1] = (kpts[:, :, 1] - dh) / r
    return boxes, scores, kpts


def xywh_to_xyxy(b):
    return np.array([b[0] - b[2] / 2, b[1] - b[3] / 2,
                     b[0] + b[2] / 2, b[1] + b[3] / 2])


def iou(a, b):
    ax1, ay1, ax2, ay2 = xywh_to_xyxy(a)
    bx1, by1, bx2, by2 = xywh_to_xyxy(b)
    ix = max(0.0, min(ax2, bx2) - max(ax1, bx1))
    iy = max(0.0, min(ay2, by2) - max(ay1, by1))
    inter = ix * iy
    union = a[2] * a[3] + b[2] * b[3] - inter
    return inter / union if union > 0 else 0.0


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    import cv2

    img_path, cpp_json = sys.argv[1], sys.argv[2]
    onnx_path = sys.argv[3] if len(sys.argv) > 3 else "models/yolov8n-pose.onnx"
    bgr = cv2.imread(img_path)
    if bgr is None:
        print(f"读不到 {img_path}")
        return 2

    ref_boxes, ref_scores, ref_kpts = onnx_reference(onnx_path, bgr)
    with open(cpp_json) as f:
        cpp = json.load(f)
    people = cpp["persons"]
    print(f"ONNX 参考 {len(ref_boxes)} 人 / C++(RKNN) {len(people)} 人  "
          f"（RKNN runtime {cpp.get('runtime','?')}）")

    errs = []
    matched = 0
    for p in people:
        bx = p["box"]
        # C++ 输出的是左上角 + 宽高，统一成 (cx, cy, w, h) 再算 IoU
        box = [bx[0] + bx[2] / 2.0, bx[1] + bx[3] / 2.0, bx[2], bx[3]]
        best_i, best_iou = -1, 0.0
        for i, rb in enumerate(ref_boxes):
            v = iou(box, rb)
            if v > best_iou:
                best_i, best_iou = i, v
        if best_iou < 0.3:
            continue
        matched += 1
        kp = np.array([[k["x"], k["y"]] for k in p["keypoints"]])
        ref = ref_kpts[best_i][:, :2]
        d = np.linalg.norm(kp - ref, axis=1)
        errs.append(d)
        print(f"  匹配 IoU={best_iou:.3f} score={p['score']:.3f}  "
              f"平均误差 {d.mean():.1f}px  最大 {d.max():.1f}px")

    if not errs:
        print("!! 没有任何人体匹配成功（IoU<0.3），解码多半有问题")
        return 1
    all_err = np.concatenate(errs)
    rate = matched / max(len(ref_boxes), len(people))
    print(f"\n匹配率 {rate * 100:.0f}%   关键点平均误差 {all_err.mean():.1f}px   "
          f"P90 {np.percentile(all_err, 90):.1f}px   最大 {all_err.max():.1f}px")

    ok = True
    # RKNN w8a8 的 conf 通道量化很粗（scale≈0.14），同一人常被多尺度重复检出，
    # 因此人数只做宽松上限检查；真正判据是「匹配上的人体」的关键点误差。
    if len(people) > max(len(ref_boxes) * 3, len(ref_boxes) + 5):
        print("!! 检出人数远超参考值（重复框过多）")
        ok = False
    if all_err.mean() > 25.0:
        print("!! 关键点平均误差 > 25px（w8a8 量化模型的经验上限，超过说明解码有问题）")
        ok = False
    if rate < 0.5:
        print("!! 匹配率 < 50%")
        ok = False
    print("=> M2 验收:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
