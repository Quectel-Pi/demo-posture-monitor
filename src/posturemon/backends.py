"""推理后端：优先 RK3576 NPU（librknnrt.so，ctypes 直调），退化到 ONNX Runtime CPU。

为什么用 ctypes 而不是 rknn-toolkit-lite2：
  板端没有 pip、python3.13 也没有对应 wheel，librknnrt.so 却是系统自带的。
  rknn_api.h 里的 5 个结构体 + 5 个函数足够跑推理，用 ctypes 直接绑，零安装。
"""
from __future__ import annotations

import ctypes
import ctypes.util
import os
import time

import numpy as np
import numpy.ctypeslib          # ★ 必须在主线程提前 import：numpy 2.x 把 ctypeslib 做成惰性属性，
                                #   推理线程里首次访问会触发 import，与采集线程并发时直接 segfault（踩过）

from . import yolopose

MAX_DIMS = 16
MAX_NAME = 256


class _Attr(ctypes.Structure):
    _fields_ = [("index", ctypes.c_uint32), ("n_dims", ctypes.c_uint32),
                ("dims", ctypes.c_uint32 * MAX_DIMS), ("name", ctypes.c_char * MAX_NAME),
                ("n_elems", ctypes.c_uint32), ("size", ctypes.c_uint32),
                ("fmt", ctypes.c_int), ("type", ctypes.c_int), ("qnt_type", ctypes.c_int),
                ("fl", ctypes.c_int8), ("zp", ctypes.c_int32), ("scale", ctypes.c_float),
                ("w_stride", ctypes.c_uint32), ("size_with_stride", ctypes.c_uint32),
                ("pass_through", ctypes.c_uint8), ("h_stride", ctypes.c_uint32)]


class _RknnInput(ctypes.Structure):
    _fields_ = [("index", ctypes.c_uint32), ("buf", ctypes.c_void_p),
                ("size", ctypes.c_uint32), ("pass_through", ctypes.c_uint8),
                ("type", ctypes.c_int), ("fmt", ctypes.c_int)]


class _RknnOutput(ctypes.Structure):
    _fields_ = [("want_float", ctypes.c_uint8), ("is_prealloc", ctypes.c_uint8),
                ("index", ctypes.c_uint32), ("buf", ctypes.c_void_p),
                ("size", ctypes.c_uint32)]


# rknn_tensor_type
T_UINT8 = 3
# rknn_tensor_format
F_NHWC = 1
# rknn_query_cmd
Q_IN_OUT_NUM, Q_INPUT_ATTR, Q_OUTPUT_ATTR, Q_SDK_VERSION = 0, 1, 2, 5


class PoseResult:
    """一帧的推理结果。"""
    __slots__ = ("boxes", "scores", "kpts", "latency_ms", "backend", "ts")

    def __init__(self, boxes, scores, kpts, latency_ms, backend, ts=None):
        self.boxes = boxes
        self.scores = scores
        self.kpts = kpts
        self.latency_ms = latency_ms
        self.backend = backend
        self.ts = ts if ts is not None else time.time()

    def __len__(self):
        return len(self.boxes)

    @property
    def trusts_kpt_conf(self):
        # RKNN w8a8 把 17x3 的关键点张量整体量化（scale≈3.19），conf 通道只剩 0..1 个量化级，
        # 实测恒为 0 —— 不能拿它当置信度用。
        return self.backend != "rknn"


class RknnBackend:
    name = "rknn"

    def __init__(self, model_path: str, lib: str = "/usr/lib/librknnrt.so"):
        self.lib_path = lib
        self.model_path = model_path
        if not os.path.exists(model_path):
            raise FileNotFoundError(model_path)
        self.L = ctypes.CDLL(lib)
        self._model = open(model_path, "rb").read()      # 必须持有，rknn 不拷贝
        self.ctx = ctypes.c_uint64(0)
        ret = self.L.rknn_init(ctypes.byref(self.ctx), self._model,
                               ctypes.c_uint32(len(self._model)), ctypes.c_uint32(0), None)
        if ret != 0:
            raise RuntimeError(f"rknn_init 失败: {ret}")
        buf = (ctypes.c_uint32 * 2)()
        self.L.rknn_query(self.ctx, Q_IN_OUT_NUM, buf, ctypes.c_size_t(ctypes.sizeof(buf)))
        self.n_in, self.n_out = int(buf[0]), int(buf[1])
        a = _Attr()
        a.index = 0
        self.L.rknn_query(self.ctx, Q_INPUT_ATTR, ctypes.byref(a),
                          ctypes.c_size_t(ctypes.sizeof(a)))
        self.in_h, self.in_w = int(a.dims[1]), int(a.dims[2])
        self.outs = []
        for i in range(self.n_out):
            o = _Attr()
            o.index = i
            self.L.rknn_query(self.ctx, Q_OUTPUT_ATTR, ctypes.byref(o),
                              ctypes.c_size_t(ctypes.sizeof(o)))
            self.outs.append(tuple(int(o.dims[j]) for j in range(o.n_dims)))
        v = (ctypes.c_char * 256)()
        v2 = (ctypes.c_char * 256)()
        self.sdk = "?"
        if self.L.rknn_query(self.ctx, Q_SDK_VERSION, ctypes.byref(v), ctypes.c_size_t(512)) == 0:
            self.sdk = v.value.decode(errors="replace")
        del v2

    def infer(self, rgb: np.ndarray, conf_th=0.35, iou_th=0.45) -> PoseResult:
        t0 = time.perf_counter()
        img = np.ascontiguousarray(rgb, dtype=np.uint8)
        if img.shape[:2] != (self.in_h, self.in_w):
            raise ValueError(f"输入尺寸 {img.shape[:2]} != 模型 {self.in_h}x{self.in_w}")
        i0 = _RknnInput(index=0, buf=img.ctypes.data, size=img.nbytes,
                        pass_through=0, type=T_UINT8, fmt=F_NHWC)
        if self.L.rknn_inputs_set(self.ctx, ctypes.c_uint32(1), ctypes.byref(i0)) != 0:
            raise RuntimeError("rknn_inputs_set 失败")
        if self.L.rknn_run(self.ctx, None) != 0:
            raise RuntimeError("rknn_run 失败")
        arr = (_RknnOutput * self.n_out)()
        for i in range(self.n_out):
            arr[i].index = i
            arr[i].want_float = 1
            arr[i].is_prealloc = 0
            arr[i].buf = None
            arr[i].size = 0
        if self.L.rknn_outputs_get(self.ctx, ctypes.c_uint32(self.n_out), arr, None) != 0:
            raise RuntimeError("rknn_outputs_get 失败")
        outs = []
        try:
            for i in range(self.n_out):
                n = int(np.prod(self.outs[i]))
                ptr = ctypes.cast(arr[i].buf, ctypes.POINTER(ctypes.c_float))
                outs.append(np.ctypeslib.as_array(ptr, shape=(n,)).copy())
        finally:
            self.L.rknn_outputs_release(self.ctx, ctypes.c_uint32(self.n_out), arr)
        boxes, scores, kpts = yolopose.decode_rknn(outs, conf_th, iou_th)
        return PoseResult(boxes, scores, kpts, (time.perf_counter() - t0) * 1000.0, "rknn")

    def close(self):
        try:
            self.L.rknn_destroy(self.ctx)
        except Exception:
            pass


class OnnxBackend:
    name = "onnx"

    def __init__(self, model_path: str, threads: int = 6):
        import onnxruntime as ort
        if not os.path.exists(model_path):
            raise FileNotFoundError(model_path)
        so = ort.SessionOptions()
        so.intra_op_num_threads = int(threads)
        so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
        self.sess = ort.InferenceSession(model_path, so,
                                         providers=["CPUExecutionProvider"])
        self.in_h = self.in_w = yolopose.IMG_SIZE
        self.sdk = f"onnxruntime {ort.__version__}"

    def infer(self, rgb: np.ndarray, conf_th=0.35, iou_th=0.45) -> PoseResult:
        t0 = time.perf_counter()
        x = (np.ascontiguousarray(rgb, dtype=np.float32) / 255.0).transpose(2, 0, 1)[None]
        out = self.sess.run(None, {self.sess.get_inputs()[0].name: x})[0]
        boxes, scores, kpts = yolopose.decode_onnx(out, conf_th, iou_th)
        return PoseResult(boxes, scores, kpts, (time.perf_counter() - t0) * 1000.0, "onnx")

    def close(self):
        pass


def make_backend(cfg):
    """按配置创建后端，rknn 失败自动退化到 onnx。返回 (backend, 说明)"""
    order = [cfg.backend] if cfg.backend in ("rknn", "onnx") else ["rknn", "onnx"]
    if cfg.backend == "auto":
        order = ["rknn", "onnx"]
    errors = []
    for kind in order:
        try:
            if kind == "rknn":
                return RknnBackend(cfg.model_rknn), "rknn(NPU)"
            return OnnxBackend(cfg.model_onnx), "onnx(CPU)"
        except Exception as e:                     # noqa: BLE001
            errors.append(f"{kind}: {type(e).__name__}: {e}")
    raise RuntimeError("没有可用的推理后端；" + " | ".join(errors))
