#!/usr/bin/env python3
"""坐姿检测与护眼提醒 —— 桌面全屏界面

对应《RK3576 坐姿检测与护眼提醒_完整实施方案》§13.3 的「两个进程」设计：

    posture-guard (systemd 系统服务, 无图形会话)
          │ AF_UNIX /run/posture-guard/status.sock，JSON 行
          ▼
    posture_guard_ui.py (本文件, 跑在登录用户会话里, GTK4)

为什么不让后台服务自己弹窗：systemd 系统服务没有 DISPLAY/WAYLAND_DISPLAY 权限，
在自动登录/多用户/无屏场景下会很不稳定（§13.3 明确要求拆成两个进程）。

本界面只接收「状态 + 17 个归一化关键点」，不接收原始画面 —— 因此不需要传输、
缓存任何图片（§18.2 隐私要求）。左边画的骨架是本地重绘的示意图形。

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
window, .root { background-color: #0b0e13; }
.panel { background-color: #11151c; padding: 20px 22px; }
.title { font-size: 14px; font-weight: 700; color: #8fa3bd; letter-spacing: 1px; }
.state-ok   { font-size: 46px; font-weight: 800; color: #4fd17a; }
.state-bad  { font-size: 46px; font-weight: 800; color: #ff5f5f; }
.state-warn { font-size: 46px; font-weight: 800; color: #ffbb4d; }
.state-idle { font-size: 46px; font-weight: 800; color: #6b7a90; }
.state-cal  { font-size: 46px; font-weight: 800; color: #58b6ff; }
.metric-key { font-size: 12px; color: #7c8ea6; }
.metric-val { font-size: 24px; font-weight: 700; color: #e8eef7; }
.metric-val-bad { font-size: 24px; font-weight: 700; color: #ff7a7a; }
.kv { font-size: 14px; color: #b9c6d6; }
.small { font-size: 11px; color: #64748b; }
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
.btn { font-size: 16px; padding: 10px 22px; border-radius: 8px;
       background-color: #1d2634; color: #cfe0f5; }
.btn-hot { background-color: #2e6bd9; color: #ffffff; }
.sep { background-color: #1b212b; min-height: 1px; }
.footer { font-size: 11px; color: #4b5768; }
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


# ---------------------------------------------------------------------------
class SocketClient:
    """后台线程持续读 status socket，把最新快照交给回调。"""

    def __init__(self, path: str, on_status, on_event=None):
        self.path = path
        self.on_status = on_status
        self.on_event = on_event
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
                        if msg.get("type") == "status":
                            self.on_status(msg)
                        elif msg.get("type") == "notification" and self.on_event:
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
    from gi.repository import Gdk, GLib, Gtk

    state = {
        "snapshot": {},
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
            self.set_decorated(not windowed)
            prov = Gtk.CssProvider()
            prov.load_from_data(CSS)
            Gtk.StyleContext.add_provider_for_display(
                Gdk.Display.get_default(), prov,
                Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)

            root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
            root.add_css_class("root")
            self.set_child(root)

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

            body = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=0)
            body.set_vexpand(True)
            root.append(body)

            # ---- 左：骨架画布 ----
            self.area = Gtk.DrawingArea()
            self.area.set_draw_func(self._draw_skeleton)
            self.area.set_hexpand(True)
            self.area.set_vexpand(True)
            body.append(self.area)

            # ---- 右：状态面板 ----
            panel = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
            panel.add_css_class("panel")
            panel.set_size_request(360, -1)
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
            panel.append(self._sep())

            panel.append(self._label("今日累计", "title"))
            self.lbl_today = Gtk.Label(label="--", xalign=0)
            self.lbl_today.add_css_class("kv")
            panel.append(self.lbl_today)
            panel.append(self._sep())

            panel.append(self._label("运行信息", "title"))
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

            panel.append(self._label("本界面只显示关键点骨架，不接收、不保存原始画面",
                                     "footer"))

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

            self.client = SocketClient(socket_path, self._on_status, self._on_event)
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
            self.client.stop()
            return False

        def _on_ack(self, *_):
            self.client.send({"cmd": "ack"})
            state["rest_shown"] = False
            self.rest_box.set_visible(False)

        # -- 数据回调（后台线程）--
        def _on_status(self, msg):
            GLib.idle_add(self._apply_status, msg)

        def _on_event(self, msg):
            GLib.idle_add(self._apply_event, msg)

        def _apply_event(self, msg):
            state["events"].append(msg)
            state["banner_until"] = time.time() + 6.0
            state["banner_text"] = f"{msg.get('title','提醒')}"
            state["banner_sub"] = msg.get("detail", "")
            self.banner_box.remove_css_class("banner-ok")
            self.banner_box.add_css_class("banner")
            return False

        def _apply_status(self, msg):
            state["snapshot"] = msg
            st = msg.get("posture_state", "UNKNOWN")
            cls = STATE_CLASS.get(st, "state-idle")
            for c in STATE_CLASS.values():
                self.lbl_state.remove_css_class(c)
            if msg.get("calibrating"):
                self.lbl_state.add_css_class("state-cal")
                self.lbl_state.set_text("校准中")
                self.lbl_reason.set_text(
                    f"请保持正常坐姿… {msg.get('calibration_progress', 0):.0f}%")
            else:
                self.lbl_state.add_css_class(cls)
                self.lbl_state.set_text(msg.get("posture_state_zh", st))
                self.lbl_reason.set_text(msg.get("explanation", ""))

            m = msg
            self.metric_vals["shoulder_angle_deg"].set_text(
                f"{m.get('shoulder_angle_deg', 0):.1f}°")
            self.metric_vals["shoulder_tilt_dev"].set_text(
                f"{m.get('shoulder_tilt_dev', 0):.1f}°")
            self.metric_vals["head_offset_y"].set_text(
                f"{m.get('head_offset_y', 0):.2f}")
            self.metric_vals["width_ratio"].set_text(
                f"{m.get('width_ratio', 0):.2f}")

            use = int(m.get("use_s", 0))
            self.lbl_use.set_text(f"{use // 60:02d}:{use % 60:02d}")
            self.lbl_remain.set_text(
                f"距下次休息提醒 {int(m.get('remaining_s', 0)) // 60} 分"
                f" {int(m.get('remaining_s', 0)) % 60} 秒")
            self.lbl_today.set_text(
                f"在座 {int(m.get('today_use_s', 0)) // 60} 分 · "
                f"不良 {int(m.get('today_bad_s', 0)) // 60} 分 · "
                f"提醒 {m.get('today_alerts', 0)} 次")
            self.lbl_info.set_text(
                f"{m.get('backend','?')} · {m.get('runtime_version','?')}\n"
                f"{m.get('frame_width',0)}x{m.get('frame_height',0)} · "
                f"{m.get('latency_ms',0):.1f}ms · {m.get('fps',0):.1f}fps · "
                f"人 {m.get('persons',0)}\n"
                f"模型 {str(m.get('model_sha256',''))[:16]}\n"
                f"基线 {'已校准' if m.get('has_baseline') else '未校准'}"
                + ("（免打扰中）" if m.get("snoozed") else ""))

            # 休息全屏
            if m.get("session_state") == "BREAK_DUE":
                if not state["rest_shown"]:
                    state["rest_shown"] = True
                    state["rest_deadline"] = time.time() + 20
                    self.rest_box.set_visible(True)
            else:
                if state["rest_shown"]:
                    state["rest_shown"] = False
                    self.rest_box.set_visible(False)

            self.area.queue_draw()
            return False

        # -- 200ms 定时器：倒计时 + 横幅超时 + 断连提示 --
        def _tick(self):
            now = time.time()
            if state["rest_shown"]:
                left = max(0, int(state["rest_deadline"] - now))
                self.rest_count.set_text(str(left))
            if state["banner_until"] and now > state["banner_until"]:
                state["banner_until"] = 0.0
                self.banner_box.set_visible(False)
            elif state["banner_until"]:
                self.banner_box.set_visible(True)
                self.banner_text.set_text(state["banner_text"])
                self.banner_sub.set_text(state["banner_sub"])
            if not self.client.connected:
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
            # 背景
            cr.set_source_rgb(0.043, 0.055, 0.075)
            cr.paint()

            snap = state["snapshot"]
            # 图框：保持 4:3（摄像头是 640x480），水平居中
            fw, fh = snap.get("frame_width") or 640, snap.get("frame_height") or 480
            aspect = fw / fh if fh else 1.333
            box_w = width
            box_h = box_w / aspect
            if box_h > height:
                box_h = height
                box_w = box_h * aspect
            ox = (width - box_w) / 2
            oy = (height - box_h) / 2

            cr.set_source_rgb(0.075, 0.09, 0.12)
            cr.rectangle(ox, oy, box_w, box_h)
            cr.fill()

            # ROI
            roi = snap.get("roi") or [0.1, 0.05, 0.8, 0.9]
            cr.set_source_rgba(0.35, 0.62, 1.0, 0.35)
            cr.set_line_width(1.5)
            cr.set_dash([8, 6])
            cr.rectangle(ox + roi[0] * box_w, oy + roi[1] * box_h,
                         roi[2] * box_w, roi[3] * box_h)
            cr.stroke()
            cr.set_dash([])

            kp = snap.get("keypoints") or []
            valid = snap.get("keypoints_valid") and len(kp) >= 34
            if not valid:
                cr.set_source_rgb(0.36, 0.42, 0.5)
                cr.select_font_face("Sans", 0, 0)
                cr.set_font_size(20)
                txt = "画面里没有可信的人体" if snap.get("present") is False \
                    else "等待关键点…"
                ext = cr.text_extents(txt)
                cr.move_to(width / 2 - ext.width / 2, height / 2)
                cr.show_text(txt)
                return

            pts = [(ox + kp[i * 2] * box_w, oy + kp[i * 2 + 1] * box_h)
                   for i in range(17)]
            bad = snap.get("posture_state") not in ("NORMAL", "USER_AWAY", "UNKNOWN")
            if bad:
                line_color = (1.0, 0.36, 0.36)
            elif snap.get("calibrating"):
                line_color = (0.35, 0.71, 1.0)
            else:
                line_color = (0.31, 0.82, 0.48)

            cr.set_source_rgb(*line_color)
            cr.set_line_width(3.0)
            cr.set_line_cap(1)
            for a, b in SKELETON:
                cr.move_to(*pts[a])
                cr.line_to(*pts[b])
                cr.stroke()

            # 关节点
            cr.set_source_rgb(1.0, 0.85, 0.2)
            for i, (px, py) in enumerate(pts):
                r = 5.0 if i in (0, 5, 6) else 3.5
                cr.arc(px, py, r, 0, 6.2832)
                cr.fill()

            # 头肩标注
            nose = pts[0]
            lsh, rsh = pts[5], pts[6]
            cr.set_source_rgb(0.55, 0.62, 0.72)
            cr.set_line_width(1.2)
            cr.move_to(*lsh)
            cr.line_to(*rsh)
            cr.stroke()
            mid = ((lsh[0] + rsh[0]) / 2, (lsh[1] + rsh[1]) / 2)
            cr.set_source_rgba(0.55, 0.62, 0.72, 0.6)
            cr.move_to(*mid)
            cr.line_to(*nose)
            cr.stroke()

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
        print(f"!! 状态 socket 不存在: {args.socket}\n"
              f"   后台服务没跑？ systemctl status posture-guard", file=sys.stderr)
        return 1

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
