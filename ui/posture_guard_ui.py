#!/usr/bin/env python3
"""坐姿检测与护眼提醒 —— 桌面全屏界面

对应《RK3576 坐姿检测与护眼提醒_完整实施方案》§13.3 的「两个进程」设计：

    posture-guard (systemd 系统服务, 无图形会话)
          │ AF_UNIX /run/posture-guard/status.sock，JSON 行
          ▼
    posture_guard_ui.py (本文件, 跑在登录用户会话里, GTK4)

为什么不让后台服务自己弹窗：systemd 系统服务没有 DISPLAY/WAYLAND_DISPLAY 权限，
在自动登录/多用户/无屏场景下会很不稳定（§13.3 明确要求拆成两个进程）。

本界面通过本地 socket 接收实时画面和关键点，画面只在内存中显示，不保存到磁盘。
语音使用随程序分发的中文 WAV，在桌面会话中播放，无需联网。

用法：
  python3 posture_guard_ui.py               # 全屏界面
  python3 posture_guard_ui.py --windowed    # 窗口模式（调试）
  python3 posture_guard_ui.py --probe       # 只连 socket 打一行状态后退出
"""
from __future__ import annotations

import argparse
import json
import os
import socket
import sys
import threading
import subprocess
from pathlib import Path
import math
import time

SOCKET_PATH = os.environ.get("POSTURE_SOCKET", "/run/posture-guard/status.sock")

# COCO-17 骨架连线（下标从 0 开始）
SKELETON = [
    (15, 13), (13, 11), (16, 14), (14, 12), (11, 12),
    (5, 11), (6, 12), (5, 6), (5, 7), (6, 8),
    (7, 9), (8, 10),
    (0, 1), (0, 2), (1, 3), (2, 4), (3, 5), (4, 6),
]

CSS = b"""
window, .root { background-color: #0c1422; color: #e5edf8; }
.header { padding: 16px 24px; background-color: #111e30; }
.brand { font-size: 23px; font-weight: 800; color: #eff6ff; }
.eyebrow { font-size: 12px; color: #6cd9c5; }
.body { margin: 18px; }
progressbar trough { min-height: 8px; border-radius: 5px; background: #23334a; }
progressbar progress { min-height: 8px; border-radius: 5px; background: #57d9bf; }
.canvas { background-color: #070d16; border: 1px solid #29384d; border-radius: 18px; }
.panel { background-color: #132136; padding: 20px 22px; border: 1px solid #29384d; border-radius: 18px; }
.title { font-size: 14px; font-weight: 700; color: #8fa3bd; letter-spacing: 1px; }
.state-ok   { font-size: 36px; font-weight: 800; color: #4fd17a; }
.state-bad  { font-size: 36px; font-weight: 800; color: #ff5f5f; }
.state-warn { font-size: 36px; font-weight: 800; color: #ffbb4d; }
.state-idle { font-size: 36px; font-weight: 800; color: #6b7a90; }
.state-cal  { font-size: 36px; font-weight: 800; color: #58b6ff; }
.metric-key { font-size: 13px; color: #9baec7; }
.metric-val { font-size: 24px; font-weight: 700; color: #e8eef7; }
.metric-val-bad { font-size: 24px; font-weight: 700; color: #ff7a7a; }
.kv { font-size: 14px; color: #b9c6d6; }
.small { font-size: 12px; color: #94a9c4; }
.chip { font-size: 12px; font-weight: 700; color: #0b0e13; background-color: #ffbb4d;
        padding: 3px 10px; border-radius: 10px; }
.banner { background-color: #d92b2b; padding: 10px 22px; }
.banner-ok { background-color: #1b6b3a; padding: 10px 22px; }
.banner-text { font-size: 24px; font-weight: 800; color: #ffffff; }
.banner-sub { font-size: 13px; color: #ffe6e6; }
.rest { background-color: rgba(4, 7, 12, 0.96); }
.rest-title { font-size: 18px; color: #9fb6d0; letter-spacing: 6px; }
.rest-count { font-size: 150px; font-weight: 800; color: #7fe0a4; }
.rest-hint { font-size: 24px; color: #dbe7f5; }
.btn { font-size: 14px; padding: 10px 14px; border-radius: 8px;
       background-color: #1d2634; background-image: none; border: 1px solid #405570; color: #e3edfc; }
.btn label { color: #e3edfc; }
.btn:disabled { opacity: 0.4; }
.btn:hover { background-color: #334866; }
.btn-hot { background-color: #2e6bd9; color: #ffffff; }
.sep { background-color: #1b212b; min-height: 1px; }
.footer { font-size: 12px; color: #8fa3bd; }
"""

STATE_CLASS = {
    "NORMAL": "state-ok",
    "SHOULDER_TILT": "state-bad",
    "HEAD_DOWN": "state-bad",
    "HEAD_TILT": "state-bad",
    "TOO_CLOSE": "state-warn",
    "HAND_ON_FACE": "state-bad",
    "USER_AWAY": "state-idle",
    "UNKNOWN": "state-idle",
}


class VoicePlayer:
    """Bounded pending prompt, delivery deduplication and observable playback failures."""
    KEYS = ('shoulder_tilt', 'head_down', 'head_tilt', 'too_close', 'hand_on_face', 'rest')

    def __init__(self):
        self.enabled = True
        self.process = None
        self.last_at = -100.0
        self.error = ""
        self.last_key = ""
        self.assets = Path(__file__).resolve().parent.parent / 'assets' / 'voice'
        self.pending = None
        self.pending_id = None
        self.seen_ids = []
        self.stderr_file = None
        self.retry_at = 0.0
        self.failures = 0

    def stop(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
        self.process = None
        if self.stderr_file:
            self.stderr_file.close()
            self.stderr_file = None

    def poll(self):
        if self.process is None:
            return
        code = self.process.poll()
        if code is None:
            return
        detail = ''
        if self.stderr_file:
            self.stderr_file.seek(0)
            detail = self.stderr_file.read().decode('utf-8', errors='replace').strip()
            self.stderr_file.close()
            self.stderr_file = None
        self.process = None
        if code != 0:
            self.error = f'播放器退出 {code}: {detail or "请检查音频输出"}'
            print(f'[{time.strftime("%Y-%m-%d %H:%M:%S")}] [voice] {self.error}', file=sys.stderr)
            self.failures += 1
            self.retry_at = time.monotonic() + 3
            if self.pending is None and self.failures <= 2:
                self.pending = (self.last_key, time.monotonic())
        else:
            self.error = ''
            self.failures = 0
            print(f'[{time.strftime("%Y-%m-%d %H:%M:%S")}] [voice] 播放完成 {self.last_key}', file=sys.stderr)

    def play(self, key, force=False):
        key = {'hand_face': 'hand_on_face', 'lean_close': 'too_close'}.get(key, key)
        if not self.enabled or key not in self.KEYS:
            return False
        self.poll()
        now = time.monotonic()
        if not force and (now < self.retry_at or (key != 'rest' and now - self.last_at < 2)):
            return False
        if self.process and self.process.poll() is None:
            if key == self.last_key and not force:
                return False
            self.stop()
        prompt = self.assets / (key + '.wav')
        if not prompt.is_file():
            self.error = f'语音文件缺失: {prompt.name}'
            return False
        import tempfile
        try:
            self.stderr_file = tempfile.TemporaryFile()
            self.process = subprocess.Popen(
                ['paplay', '--client-name=posture-guard', '--stream-name=posture-reminder', str(prompt)],
                stdout=subprocess.DEVNULL, stderr=self.stderr_file)
            self.last_at = now
            self.last_key = key
            self.error = ''
            print(f'[{time.strftime("%Y-%m-%d %H:%M:%S")}] [voice] 开始播放 {key}', file=sys.stderr)
            return True
        except OSError as exc:
            self.error = str(exc)
            if self.stderr_file:
                self.stderr_file.close()
                self.stderr_file = None
            self.retry_at = now + 3
            print(f'[{time.strftime("%Y-%m-%d %H:%M:%S")}] [voice] 播放失败: {exc}', file=sys.stderr)
            return False

    def request(self, key, event_id=None):
        key = {'hand_face': 'hand_on_face', 'lean_close': 'too_close'}.get(key, key)
        if not self.enabled or key not in self.KEYS:
            return False
        if event_id and event_id in self.seen_ids:
            return False
        if event_id and self.pending is not None and event_id == self.pending_id:
            return self.tick()
        self.pending = (key, time.monotonic())
        self.pending_id = event_id
        return self.tick()

    def tick(self, allowed_keys=None):
        self.poll()
        if not self.enabled:
            self.pending = None
            self.pending_id = None
            return False
        if self.pending is None:
            return False
        key, queued_at = self.pending
        if time.monotonic() - queued_at > 15 or (allowed_keys is not None and key not in allowed_keys):
            self.pending = None
            self.pending_id = None
            return False
        if self.play(key):
            if self.pending_id:
                self.seen_ids.append(self.pending_id)
                self.seen_ids = self.seen_ids[-32:]
            self.pending = None
            self.pending_id = None
            return True
        return False


# ---------------------------------------------------------------------------
class SocketClient:
    """后台线程持续读 status socket，把最新快照交给回调。"""

    def __init__(self, path: str, on_status, on_event=None, on_video=None):
        self.path = path
        self.on_status = on_status
        self.on_event = on_event
        self.on_video = on_video
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self.connected = False
        self.last_error = ""
        self.sock: socket.socket | None = None

    def start(self):
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass

    def send(self, payload: dict) -> bool:
        if not self.sock:
            return False
        try:
            self.sock.sendall((json.dumps(payload) + "\n").encode())
            return True
        except OSError:
            return False

    def _run(self):
        while not self._stop.is_set():
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.settimeout(5)
                s.connect(self.path)
                s.settimeout(None)
                self.sock = s
                self.connected = True
                self.last_error = ""
                buf = b""
                while not self._stop.is_set():
                    chunk = s.recv(16384)
                    if not chunk:
                        break
                    buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        if not line.strip():
                            continue
                        try:
                            msg = json.loads(line.decode())
                        except ValueError:
                            continue
                        kind = msg.get("type")
                        if kind == "status":
                            self.on_status(msg)
                        elif kind == "video" and self.on_video:
                            self.on_video(msg)
                        elif kind == "notification" and self.on_event:
                            self.on_event(msg)
            except OSError as exc:
                self.last_error = str(exc)
            finally:
                self.connected = False
                if self.sock:
                    try:
                        self.sock.close()
                    except OSError:
                        pass
                    self.sock = None
            if self._stop.is_set():
                break
            time.sleep(1.0)      # 服务没起来就每秒重连


# ---------------------------------------------------------------------------
def build_ui(windowed: bool, socket_path: str):
    import gi

    gi.require_version("Gtk", "4.0")
    gi.require_version("Gdk", "4.0")
    gi.require_version("PangoCairo", "1.0")
    from gi.repository import Gdk, GLib, Gtk, Pango, PangoCairo

    state = {
        "snapshot": {},
        "video": None,          # Gdk.Texture
        "video_wh": (0, 0),
        "video_seq": 0,
        "video_at": 0.0,
        "events": [],
        "rest_shown": False,
        "rest_deadline": 0.0,
        "banner_until": 0.0,
        "banner_text": "",
        "banner_sub": "",
    }

    class App(Gtk.Application):
        def __init__(self):
            # NON_UNIQUE：一体机只有一个实例，也不需要单实例转发；
            # 从 autostart 拉起时用户 D-Bus 可能还没就绪，注册会打一堆 Gtk-CRITICAL
            from gi.repository import Gio
            super().__init__(application_id="com.quectel.PostureGuardUI",
                             flags=Gio.ApplicationFlags.NON_UNIQUE)

        def do_activate(self):
            win = MainWindow(self)
            win.present()
            if not windowed:
                win.fullscreen()

    class MainWindow(Gtk.ApplicationWindow):
        def __init__(self, app):
            super().__init__(application=app, title="坐姿检测与护眼提醒一体机")
            self.set_decorated(windowed)
            self.set_default_size(1200, 800)
            self.voice = VoicePlayer()
            self.voice_muted = False
            prov = Gtk.CssProvider()
            prov.load_from_data(CSS)
            Gtk.StyleContext.add_provider_for_display(
                Gdk.Display.get_default(), prov,
                Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)

            root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
            root.add_css_class("root")
            self.set_child(root)

            header = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=18)
            header.add_css_class("header")
            brand = self._label("坐姿与护眼", "brand")
            brand.set_hexpand(True)
            header.append(brand)
            self.lbl_live = self._label("正在连接设备…", "eyebrow")
            header.append(self.lbl_live)
            root.append(header)

            # ---- 顶部横幅 ----
            self.banner_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL,
                                      spacing=2)
            self.banner_box.set_visible(False)
            root.append(self.banner_box)
            self.banner_text = Gtk.Label(label="", xalign=0)
            self.banner_text.add_css_class("banner-text")
            self.banner_sub = Gtk.Label(label="", xalign=0)
            self.banner_sub.add_css_class("banner-sub")
            self.banner_box.append(self.banner_text)
            self.banner_box.append(self.banner_sub)

            body = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=18)
            body.add_css_class("body")
            body.set_vexpand(True)
            root.append(body)

            # ---- 左：画面(Picture) + 骨架(DrawingArea 叠加) ----
            # 用 Gtk.Picture 显示视频、透明 DrawingArea 叠骨架，而不是在 cairo 里
            # 手工把 texture 画上去 —— 后者依赖 Gdk.cairo_set_source_paintable，
            # 这个 GTK 版本上没有，会直接 AttributeError（踩过）。
            self.canvas = Gtk.Overlay()
            self.canvas.add_css_class("canvas")
            self.canvas.set_hexpand(True)
            self.canvas.set_vexpand(True)
            body.append(self.canvas)

            self.picture = Gtk.Picture()
            self.picture.set_content_fit(Gtk.ContentFit.CONTAIN)
            self.picture.set_can_shrink(True)
            self.picture.set_hexpand(True)
            self.picture.set_vexpand(True)
            self.canvas.set_child(self.picture)

            self.area = Gtk.DrawingArea()
            self.area.set_draw_func(self._draw_skeleton)
            self.area.set_hexpand(True)
            self.area.set_vexpand(True)
            self.area.set_can_target(False)     # 不抢事件
            self.canvas.add_overlay(self.area)

            # ---- 右：状态面板 ----
            panel = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
            panel.add_css_class("panel")
            panel.set_size_request(330, -1)
            body.append(panel)

            panel.append(self._label("坐姿状态", "title"))
            self.lbl_state = Gtk.Label(label="等待数据", xalign=0)
            self.lbl_state.add_css_class("state-idle")
            panel.append(self.lbl_state)
            self.lbl_reason = Gtk.Label(label="", xalign=0, wrap=True)
            self.lbl_reason.add_css_class("kv")
            panel.append(self.lbl_reason)
            panel.append(self._sep())

            self.metric_vals = {}
            for key, name in (("shoulder_angle_deg", "肩线偏角"),
                              ("shoulder_tilt_dev", "偏离基准"),
                              ("head_offset_y", "头肩纵向"),
                              ("width_ratio", "肩宽占比")):
                row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL)
                k = Gtk.Label(label=name, xalign=0)
                k.add_css_class("metric-key")
                k.set_hexpand(True)
                v = Gtk.Label(label="--", xalign=1)
                v.add_css_class("metric-val")
                row.append(k)
                row.append(v)
                panel.append(row)
                self.metric_vals[key] = v
            panel.append(self._sep())

            panel.append(self._label("用眼计时", "title"))
            self.lbl_use = Gtk.Label(label="--", xalign=0)
            self.lbl_use.add_css_class("metric-val")
            panel.append(self.lbl_use)
            self.lbl_remain = Gtk.Label(label="", xalign=0)
            self.lbl_remain.add_css_class("kv")
            panel.append(self.lbl_remain)
            self.eye_progress = Gtk.ProgressBar()
            panel.append(self.eye_progress)
            panel.append(self._sep())

            panel.append(self._label("今日累计", "title"))
            self.lbl_today = Gtk.Label(label="--", xalign=0)
            self.lbl_today.add_css_class("kv")
            panel.append(self.lbl_today)
            panel.append(self._sep())

            panel.append(self._label("设备状态", "title"))
            self.lbl_info = Gtk.Label(label="--", xalign=0, wrap=True)
            self.lbl_info.add_css_class("small")
            panel.append(self.lbl_info)

            btns = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
            self.btn_cal = Gtk.Button(label="重新校准")
            self.btn_cal.add_css_class("btn")
            self.btn_cal.connect("clicked", lambda *_: self.client.send({"cmd": "calibrate"}))
            self.btn_snooze = Gtk.Button(label="忽略 15 分钟")
            self.btn_snooze.add_css_class("btn")
            self.btn_snooze.connect("clicked",
                                    lambda *_: self.client.send({"cmd": "snooze", "minutes": 15}))
            btns.append(self.btn_cal)
            btns.append(self.btn_snooze)
            panel.append(btns)

            # Voice uses the desktop audio session, avoiding system service audio access.
            voice_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
            self.btn_voice = Gtk.Button(label="语音：开启")
            self.btn_voice.add_css_class("btn")
            self.btn_voice.connect("clicked", self._toggle_voice)
            voice_test = Gtk.Button(label="试听语音")
            voice_test.add_css_class("btn")
            voice_test.connect("clicked", lambda *_: self.voice.play("head_down", force=True))
            voice_row.append(self.btn_voice)
            voice_row.append(voice_test)
            panel.append(voice_row)
            panel.append(self._label("画面仅本机显示 · 不保存录像", "footer"))

            # ---- 休息全屏遮罩 ----
            self.rest_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL,
                                    spacing=18)
            self.rest_box.add_css_class("rest")
            self.rest_box.set_visible(False)
            self.rest_box.set_halign(Gtk.Align.FILL)
            self.rest_box.set_valign(Gtk.Align.FILL)
            self.rest_box.set_vexpand(True)
            self.rest_box.set_hexpand(True)
            self.rest_title = Gtk.Label(label="护眼休息", xalign=0.5)
            self.rest_title.add_css_class("rest-title")
            self.rest_count = Gtk.Label(label="20", xalign=0.5)
            self.rest_count.add_css_class("rest-count")
            self.rest_hint = Gtk.Label(label="请看向 6 米外，让眼睛放松", xalign=0.5)
            self.rest_hint.add_css_class("rest-hint")
            self.btn_ack = Gtk.Button(label="我已经休息好了")
            self.btn_ack.add_css_class("btn")
            self.btn_ack.add_css_class("btn-hot")
            self.btn_ack.connect("clicked", self._on_ack)
            for w in (self.rest_title, self.rest_count, self.rest_hint):
                self.rest_box.append(w)
            self.rest_box.append(self.btn_ack)
            # 用 Overlay 盖住整个窗口
            overlay = Gtk.Overlay()
            root.remove(body)
            overlay.set_child(body)
            overlay.add_overlay(self.rest_box)
            root.append(overlay)
            self.rest_box.set_visible(False)

            self.client = SocketClient(socket_path, self._on_status, self._on_event,
                                       self._on_video)
            self.client.start()
            GLib.timeout_add(200, self._tick)
            self.connect("close-request", self._on_close)

        # -- helpers --
        def _label(self, text, cls):
            lbl = Gtk.Label(label=text, xalign=0)
            lbl.add_css_class(cls)
            return lbl

        def _sep(self):
            s = Gtk.Box()
            s.add_css_class("sep")
            s.set_size_request(-1, 1)
            return s

        def _on_close(self, *_):
            self.voice.stop()
            self.client.stop()
            return False

        def _toggle_voice(self, *_):
            self.voice_muted = not self.voice_muted
            self.voice.enabled = not self.voice_muted and state["snapshot"].get("voice_enabled", False)
            self.btn_voice.set_label("语音：开启" if self.voice.enabled else "语音：静音")
            if not self.voice.enabled:
                self.voice.stop()

        def _on_ack(self, *_):
            # 给用户即时反馈：命令是异步的，若 socket 断了 send() 返回 False，
            # 以前这里静默无视，用户会以为按钮坏了（实际踩过：服务端解析失败时
            # 按钮同样毫无反应，因为两端都不报错）。
            ok = self.client.send({"cmd": "ack"})
            if ok:
                state["rest_shown"] = False
                self.rest_box.set_visible(False)
                self.lbl_state.set_text("已确认休息，开始新一轮计时")
            else:
                self.rest_hint.set_text("⚠ 与后台服务断开，确认未发送（请检查服务是否在跑）")

        # -- 数据回调（后台线程）--
        def _on_status(self, msg):
            GLib.idle_add(self._apply_status, msg)

        def _on_event(self, msg):
            GLib.idle_add(self._apply_event, msg)

        def _on_video(self, msg):
            # base64 -> JPEG bytes -> Gdk.Texture。放在主线程做，
            # 因为 Gdk 对象不保证线程安全；单帧解码 ~2ms，10fps 完全够
            GLib.idle_add(self._apply_video, msg)

        def _apply_event(self, msg):
            self.voice.request(msg.get("key", ""), msg.get("id"))
            state["banner_until"] = time.monotonic() + 6.0
            state["banner_text"] = f"{msg.get('title','提醒')}"
            state["banner_sub"] = msg.get("detail", "")
            self.banner_box.remove_css_class("banner-ok")
            self.banner_box.add_css_class("banner")
            return False

        def _apply_video(self, msg):
            import base64
            try:
                raw = base64.b64decode(msg["jpeg"])
                texture = Gdk.Texture.new_from_bytes(GLib.Bytes.new(raw))
            except Exception as exc:      # 不要静默，否则"没有画面"根本查不出来
                print(f"[ui] 视频帧解码失败: {exc!r}", file=sys.stderr)
                return False
            state["video"] = texture
            state["video_wh"] = (int(msg.get("w", 0)), int(msg.get("h", 0)))
            state["video_seq"] = int(msg.get("seq", 0))
            state["video_at"] = time.monotonic()
            self.picture.set_paintable(texture)
            self.area.queue_draw()
            return False

        def _apply_status(self, msg):
            state["snapshot"] = msg
            self.voice.enabled = msg.get("voice_enabled", False) and not self.voice_muted
            self.btn_voice.set_label("语音：开启" if self.voice.enabled else "语音：静音")
            self.lbl_live.set_text(f"● 实时监测  ·  {msg.get('fps', 0):.1f} FPS")
            # Status carries the latest notification too: recover an event lost during
            # startup/reconnect, but never speak an old posture after the user corrected it.
            current_key = {"SHOULDER_TILT":"shoulder_tilt", "HEAD_DOWN":"head_down",
                           "HEAD_TILT":"head_tilt", "TOO_CLOSE":"too_close",
                           "HAND_ON_FACE":"hand_on_face"}.get(msg.get("posture_state"))
            if msg.get("session_state") == "BREAK_DUE":
                current_key = "rest"
            event_key = msg.get("last_notification_key")
            age = msg.get("last_notification_ago_s", -1)
            frames_ready = (current_key == "rest" or
                            msg.get("bad_posture_frames", 0) >= msg.get("posture_reminder_frames", 1))
            if event_key == current_key and frames_ready and 0 <= age < 60 and msg.get("last_notification_id") and not msg.get("snoozed"):
                self.voice.request(event_key, msg["last_notification_id"])
            self.voice.tick([current_key] if current_key else [])
            st = msg.get("posture_state", "UNKNOWN")
            cls = STATE_CLASS.get(st, "state-idle")
            for c in set(STATE_CLASS.values()) | {"state-cal"}:
                self.lbl_state.remove_css_class(c)
            if msg.get("calibrating"):
                self.lbl_state.add_css_class("state-cal")
                self.lbl_state.set_text("校准中")
                self.lbl_reason.set_text(
                    f"请保持正常坐姿… {msg.get('calibration_progress', 0):.0f}%")
            else:
                self.lbl_state.add_css_class(cls)
                confirming = st == "UNKNOWN" and msg.get("explanation", "").startswith("画像判定中")
                self.lbl_state.set_text("正在确认坐姿" if confirming else msg.get("posture_state_zh", st))
                self.lbl_reason.set_text("请保持自然坐姿，让头部和双肩完整入镜" if confirming else msg.get("explanation", ""))

            m = msg
            self.metric_vals["shoulder_angle_deg"].set_text(
                f"{m.get('shoulder_angle_deg', 0):.1f}°")
            self.metric_vals["shoulder_tilt_dev"].set_text(
                f"{m.get('shoulder_tilt_dev', 0):.1f}°")
            self.metric_vals["head_offset_y"].set_text(
                f"{m.get('head_offset_y', 0):.2f}")
            self.metric_vals["width_ratio"].set_text(
                f"{m.get('width_ratio', 0):.2f}")

            if not m.get("metrics_valid"):
                for value in self.metric_vals.values():
                    value.set_text("—")

            use = int(m.get("use_s", 0))
            total = max(1, use + float(m.get("remaining_s", 0)))
            self.eye_progress.set_fraction(min(1.0, use / total))
            self.lbl_use.set_text(f"{use // 60:02d}:{use % 60:02d}")
            self.lbl_remain.set_text(
                f"距下次休息提醒 {int(m.get('remaining_s', 0)) // 60} 分"
                f" {int(m.get('remaining_s', 0)) % 60} 秒")
            self.lbl_today.set_text(
                f"在座 {int(m.get('today_use_s', 0)) // 60} 分 · "
                f"不良 {int(m.get('today_bad_s', 0)) // 60} 分 · "
                f"提醒 {m.get('today_alerts', 0)} 次")
            self.lbl_info.set_text(
                f"NPU 推理 {m.get('latency_ms', 0):.1f} ms  ·  {m.get('fps', 0):.1f} FPS\n"
                f"{'个人基准已就绪' if m.get('has_baseline') else '请坐正并完成校准'}"
                + (" · 免打扰中" if m.get("snoozed") else "")
                + (f"\n连续不良 {m.get('bad_posture_frames', 0)}/{m.get('posture_reminder_frames')} 帧 · "
                   + {"waiting_frames":"等待确认", "normal":"坐姿正常", "invalid_pose":"数据不足",
                      "cooldown":f"已提醒，冷却剩余 {math.ceil(m.get('posture_reminder_cooldown_s', 0))} 秒", "snoozed":"免打扰", "notified":"已发出提醒"}.get(m.get("posture_reminder_reason"), "监测中")
                   if m.get("posture_reminder_frames", 0) else ""))
            self.lbl_reason.set_text(m.get("baseline_hint") or self.lbl_reason.get_text())

            # 休息全屏
            if m.get("session_state") == "BREAK_DUE":
                state["rest_deadline"] = time.monotonic() + float(m.get("rest_remaining_s", 20))
                if not state["rest_shown"]:
                    state["rest_shown"] = True
                    state["rest_deadline"] = time.monotonic() + float(m.get("rest_remaining_s", m.get("far_look_s", 20)))
                    if not m.get("snoozed"):
                        self.voice.play("rest")
                    self.rest_box.set_visible(True)
            else:
                if state["rest_shown"]:
                    state["rest_shown"] = False
                    self.rest_box.set_visible(False)

            self.area.queue_draw()
            return False

        # -- 200ms 定时器：倒计时 + 横幅超时 + 断连提示 --
        def _tick(self):
            now = time.monotonic()
            snap = state["snapshot"]
            current_key = {"SHOULDER_TILT":"shoulder_tilt", "HEAD_DOWN":"head_down",
                           "HEAD_TILT":"head_tilt", "TOO_CLOSE":"too_close",
                           "HAND_ON_FACE":"hand_on_face"}.get(snap.get("posture_state"))
            if snap.get("session_state") == "BREAK_DUE":
                current_key = "rest"
            self.voice.tick([current_key] if current_key else [])
            if self.voice.error:
                self.btn_voice.set_label("语音：播放失败")
                self.btn_voice.set_tooltip_text(self.voice.error)
            elif self.voice.pending:
                self.btn_voice.set_label("语音：等待播放")
            self.btn_voice.set_tooltip_text(self.voice.error or "当前音频输出由桌面声音设置决定")
            if state["rest_shown"]:
                left = max(0, math.ceil(state["rest_deadline"] - now))
                self.btn_ack.set_sensitive(left == 0)
                self.rest_hint.set_text("请看向 6 米外，让眼睛放松" if left else "远眺计时完成，休息后请确认")
                self.rest_count.set_text(str(left))
            if state["banner_until"] and now > state["banner_until"]:
                state["banner_until"] = 0.0
                self.banner_box.set_visible(False)
            elif state["banner_until"]:
                self.banner_box.set_visible(True)
                self.banner_text.set_text(state["banner_text"])
                self.banner_sub.set_text(state["banner_sub"])
            # 视频停更检测（摄像头掉线时会停在最后一帧，必须显式提示）
            if state["video"] is not None and state["video_at"]:
                stale = now - state["video_at"]
                if stale > 3.0:
                    self.lbl_remain.set_text(f"⚠ 摄像头画面已停更 {int(stale)}s（检查设备/占用）")
            if not self.client.connected:
                self.lbl_live.set_text("● 设备连接已断开")
                self.lbl_state.set_text("服务未连接")
                for c in STATE_CLASS.values():
                    self.lbl_state.remove_css_class(c)
                self.lbl_state.add_css_class("state-idle")
                self.lbl_reason.set_text(
                    f"连不上 {socket_path}\n{self.client.last_error}\n"
                    "（systemctl status posture-guard 看看后台服务）")
            return True

        # -- 骨架绘制 --
        def _draw_skeleton(self, area, cr, width, height):
            """只画叠加层（ROI/骨架/提示）。视频由下面的 Gtk.Picture 负责。

            DrawingArea 是透明的，所以这里**不能**填充背景色，否则会盖住画面。
            """
            def draw_text(text, center_x, y, size=16):
                layout = PangoCairo.create_layout(cr)
                layout.set_font_description(Pango.FontDescription(f"Sans {size}"))
                layout.set_text(text, -1)
                tw, th = layout.get_pixel_size()
                cr.move_to(center_x - tw / 2, y)
                PangoCairo.show_layout(cr, layout)

            snap = state["snapshot"]
            has_video = state["video"] is not None

            # 与 Gtk.Picture(content_fit=CONTAIN) 完全一致的几何：等比缩放 + 居中
            if has_video and state["video_wh"][1]:
                fw, fh = state["video_wh"]
            else:
                fw = snap.get("frame_width") or 640
                fh = snap.get("frame_height") or 480
            if not fw or not fh:
                return
            scale = min(width / fw, height / fh)
            box_w, box_h = fw * scale, fh * scale
            ox, oy = (width - box_w) / 2, (height - box_h) / 2

            if not has_video:
                # 没有视频流时才画一块底色 + 明确提示，避免用户以为摄像头坏了
                cr.set_source_rgb(0.02, 0.028, 0.04)
                cr.rectangle(ox, oy, box_w, box_h)
                cr.fill()
                cr.set_source_rgb(0.62, 0.48, 0.28)
                cr.select_font_face("Sans", 0, 0)
                cr.set_font_size(18)
                txt = "视频流未开启（ui.stream_video=false），仅显示骨架" if snap \
                    else "等待摄像头画面…"
                draw_text(txt, ox + box_w / 2, oy + box_h / 2)

            # ---- ROI 框 ----
            roi = snap.get("roi") or [0.1, 0.05, 0.8, 0.9]
            cr.set_source_rgba(0.35, 0.62, 1.0, 0.40)
            cr.set_line_width(1.5)
            cr.set_dash([8, 6])
            cr.rectangle(ox + roi[0] * box_w, oy + roi[1] * box_h,
                         roi[2] * box_w, roi[3] * box_h)
            cr.stroke()
            cr.set_dash([])

            kp = snap.get("keypoints") or []
            valid = bool(snap.get("keypoints_valid")) and len(kp) >= 34
            if not valid:
                cr.set_source_rgba(0.95, 0.85, 0.65, 0.9)
                cr.select_font_face("Sans", 0, 0)
                cr.set_font_size(20)
                txt = "画面里没有可信的人体" if snap.get("present") is False \
                    else "等待关键点…"
                draw_text(txt, ox + box_w / 2, oy + box_h - 42, 14)
                return

            pts = [(ox + kp[i * 2] * box_w, oy + kp[i * 2 + 1] * box_h)
                   for i in range(17)]
            bad = snap.get("posture_state") not in ("NORMAL", "USER_AWAY", "UNKNOWN")
            if bad:
                line_color = (1.0, 0.32, 0.32)
            elif snap.get("calibrating"):
                line_color = (0.35, 0.71, 1.0)
            else:
                line_color = (0.31, 0.90, 0.52)

            # 描边：先画一层深色宽线，再画亮色细线，保证在任意画面上都看得清
            for pass_i in (0, 1):
                if pass_i == 0:
                    cr.set_source_rgba(0, 0, 0, 0.55)
                    cr.set_line_width(6.0)
                else:
                    cr.set_source_rgb(*line_color)
                    cr.set_line_width(3.0)
                cr.set_line_cap(1)
                for a, b in SKELETON:
                    cr.move_to(*pts[a])
                    cr.line_to(*pts[b])
                    cr.stroke()

            # 关节点
            for i, (px, py) in enumerate(pts):
                r = 5.0 if i in (0, 5, 6) else 3.5
                cr.set_source_rgba(0, 0, 0, 0.6)
                cr.arc(px, py, r + 1.5, 0, 6.2832)
                cr.fill()
                cr.set_source_rgb(1.0, 0.86, 0.20)
                cr.arc(px, py, r, 0, 6.2832)
                cr.fill()

            # 肩线与头轴参考线
            nose, lsh, rsh = pts[0], pts[5], pts[6]
            cr.set_source_rgba(0.75, 0.82, 0.92, 0.75)
            cr.set_line_width(1.4)
            cr.move_to(*lsh)
            cr.line_to(*rsh)
            cr.stroke()
            mid = ((lsh[0] + rsh[0]) / 2, (lsh[1] + rsh[1]) / 2)
            cr.set_dash([5, 4])
            cr.move_to(*mid)
            cr.line_to(*nose)
            cr.stroke()
            cr.set_dash([])

            # 左下角标出画面尺寸与帧号，方便现场判断"画面是不是活的"
            cr.set_source_rgba(0.75, 0.82, 0.92, 0.65)
            cr.select_font_face("Sans", 0, 0)
            cr.set_font_size(12)
            cr.move_to(ox + 10, oy + box_h - 10)
            cr.show_text(f"{fw}x{fh}  seq={state.get('video_seq', 0)}  "
                         f"fps={snap.get('fps', 0):.1f}  "
                         f"{snap.get('latency_ms', 0):.1f}ms")

    app = App()
    return app, state


def probe(socket_path: str) -> int:
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect(socket_path)
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(8192)
            if not chunk:
                break
            buf += chunk
        print(buf.split(b"\n")[0].decode())
        return 0
    except OSError as exc:
        print(f"连不上 {socket_path}: {exc}", file=sys.stderr)
        return 1


def main():
    ap = argparse.ArgumentParser(description="坐姿检测与护眼提醒 桌面界面")
    ap.add_argument("--windowed", action="store_true", help="窗口模式（调试）")
    ap.add_argument("--probe", action="store_true", help="只读一帧状态后退出")
    ap.add_argument("--socket", default=SOCKET_PATH, help="状态 socket 路径")
    args = ap.parse_args()

    if args.probe:
        return probe(args.socket)
    if not os.path.exists(args.socket):
        print(f"[ui] 等待后台服务 {args.socket}，界面将自动重连", file=sys.stderr)

    # RK3576 上 Mesa 的 Zink/Vulkan 后端会报 vkCreateInstance 失败，
    # 但应用仍能起来；显式选 ngl 可少一堆噪声日志（见 docs/troubleshooting.md）
    os.environ.setdefault("GSK_RENDERER", "ngl")

    import gi

    gi.require_version("Gtk", "4.0")
    from gi.repository import GLib  # noqa: F401

    app, state = build_ui(args.windowed, args.socket)

    return app.run([])


if __name__ == "__main__":
    sys.exit(main())
