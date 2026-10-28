"""GTK4 全屏界面：左侧实时画面 + 骨架，右侧状态面板，顶部告警横幅。"""
from __future__ import annotations

import json
import math
import os
import subprocess
import time

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gdk, GLib, Gtk  # noqa: E402

from . import overlay, rules  # noqa: E402
from .stats import fmt_dur  # noqa: E402

CSS = b"""
window, .root { background-color: #0d1015; }
.panel { background-color: #12161d; padding: 18px 20px; }
.title { font-size: 15px; font-weight: 700; color: #8fa3bd; letter-spacing: 1px; }
.state-ok   { font-size: 40px; font-weight: 800; color: #4fd17a; }
.state-bad  { font-size: 40px; font-weight: 800; color: #ff5f5f; }
.state-idle { font-size: 40px; font-weight: 800; color: #6b7a90; }
.metric-key { font-size: 13px; color: #7c8ea6; }
.metric-val { font-size: 26px; font-weight: 700; color: #e8eef7; }
.metric-val-bad { font-size: 26px; font-weight: 700; color: #ff7a7a; }
.kv { font-size: 14px; color: #b9c6d6; }
.small { font-size: 12px; color: #6b7a90; }
.banner { background-color: #d92b2b; padding: 10px 22px; }
.banner-text { font-size: 24px; font-weight: 800; color: #ffffff; }
.banner-sub { font-size: 14px; color: #ffe0e0; }
.rest { background-color: rgba(4, 8, 14, 0.94); }
.rest-title { font-size: 20px; color: #9fb6d0; letter-spacing: 4px; }
.rest-count { font-size: 140px; font-weight: 800; color: #7fe0a4; }
.rest-hint { font-size: 26px; color: #dbe7f5; }
.sep { background-color: #1e2530; min-height: 1px; }
"""


class PostureWindow(Gtk.ApplicationWindow):
    def __init__(self, app, cfg, backend, engine, stats, backend_desc, sdk):
        super().__init__(application=app, title="坐姿检测与护眼提醒一体机")
        self.cfg = cfg
        self.backend = backend
        self.engine = engine
        self.stats = stats
        self.backend_desc = backend_desc
        self.sdk = sdk

        # 运行时状态
        self.frame = None
        self.frame_seq = -1
        self.pose = None                  # backends.PoseResult
        self.metrics = None
        self.map_info = None              # (r, dw, dh) 用于坐标换算
        self.fps = 0.0
        self._frame_times: list[float] = []
        self.alert_until = 0.0
        self.alert_text = ""
        self.alert_sub = ""
        self.rest_until = 0.0
        self.seated_since = 0.0
        self.next_eye_rest = time.time() + float(cfg.eye_rest_interval_s)
        self.last_remind = 0.0
        self.next_sedentary = time.time() + float(cfg.sedentary_interval_s)
        self.surface = overlay.FrameSurface()
        self._quit_armed = 0.0

        self.set_decorated(False)
        if cfg.fullscreen:
            self.fullscreen()
        self.set_default_size(1280, 720)

        self._build_ui()
        self._install_css()
        self._install_keys()

        GLib.timeout_add(33, self._tick_ui)          # ~30fps 刷新
        GLib.timeout_add(500, self._tick_panel)      # 面板/计时

    # ---------------- UI ----------------
    def _install_css(self):
        prov = Gtk.CssProvider()
        prov.load_from_data(CSS)
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(), prov, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)

    def _build_ui(self):
        root = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL)
        root.add_css_class("root")

        # ---- 左：视频 + overlay ----
        self.area = Gtk.DrawingArea()
        self.area.set_hexpand(True)
        self.area.set_vexpand(True)
        self.area.set_draw_func(self._on_draw)

        self.banner = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.banner.add_css_class("banner")
        self.banner.set_valign(Gtk.Align.START)
        self.banner.set_visible(False)
        self.banner_text = Gtk.Label(label="")
        self.banner_text.add_css_class("banner-text")
        self.banner_sub = Gtk.Label(label="")
        self.banner_sub.add_css_class("banner-sub")
        self.banner.append(self.banner_text)
        self.banner.append(self.banner_sub)

        self.rest_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.rest_box.add_css_class("rest")
        self.rest_box.set_halign(Gtk.Align.FILL)
        self.rest_box.set_valign(Gtk.Align.FILL)
        self.rest_box.set_visible(False)
        rt = Gtk.Label(label="护 眼 休 息")
        rt.add_css_class("rest-title")
        rt.set_vexpand(True)
        rt.set_valign(Gtk.Align.END)
        self.rest_count = Gtk.Label(label="20")
        self.rest_count.add_css_class("rest-count")
        rh = Gtk.Label(label="抬头看 6 米外的远处，放松眼睛")
        rh.add_css_class("rest-hint")
        rh.set_vexpand(True)
        rh.set_valign(Gtk.Align.START)
        for wdg in (rt, self.rest_count, rh):
            self.rest_box.append(wdg)

        ov = Gtk.Overlay()
        ov.set_child(self.area)
        ov.add_overlay(self.banner)
        ov.add_overlay(self.rest_box)
        ov.set_hexpand(True)
        ov.set_vexpand(True)

        # ---- 右：面板 ----
        panel = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        panel.add_css_class("panel")
        panel.set_size_request(300, -1)

        t = Gtk.Label(label="坐姿监测", xalign=0)
        t.add_css_class("title")
        panel.append(t)

        self.lbl_state = Gtk.Label(label="等待入座", xalign=0)
        self.lbl_state.add_css_class("state-idle")
        panel.append(self.lbl_state)

        self.lbl_reason = Gtk.Label(label="", xalign=0)
        self.lbl_reason.add_css_class("kv")
        panel.append(self.lbl_reason)
        panel.append(self._sep())

        # 时长三件套
        grid = Gtk.Grid(column_spacing=14, row_spacing=2)
        self.lbl_cont = self._metric(grid, 0, "连续不良")
        self.lbl_today = self._metric(grid, 1, "今日不良")
        self.lbl_seated = self._metric(grid, 2, "连续坐姿")
        panel.append(grid)
        panel.append(self._sep())

        # 违规计数
        cnt_title = Gtk.Label(label="今日违规次数", xalign=0)
        cnt_title.add_css_class("small")
        panel.append(cnt_title)
        self.count_labels = {}
        cgrid = Gtk.Grid(column_spacing=12, row_spacing=3)
        for i, key in enumerate(rules.VIOLATION_PRIORITY):
            name = Gtk.Label(label=rules.VIOLATION_LABEL[key], xalign=0)
            name.add_css_class("kv")
            val = Gtk.Label(label="0", xalign=1)
            val.add_css_class("kv")
            cgrid.attach(name, 0, i, 1, 1)
            cgrid.attach(val, 1, i, 1, 1)
            self.count_labels[key] = val
        panel.append(cgrid)
        panel.append(self._sep())

        self.lbl_eye = Gtk.Label(label="护眼提醒 20-20-20", xalign=0)
        self.lbl_eye.add_css_class("small")
        panel.append(self.lbl_eye)
        self.lbl_eye_val = Gtk.Label(label="--:--", xalign=0)
        self.lbl_eye_val.add_css_class("kv")
        panel.append(self.lbl_eye_val)
        panel.append(self._sep())

        self.lbl_hint = Gtk.Label(label="", xalign=0)
        self.lbl_hint.add_css_class("small")
        self.lbl_hint.set_wrap(True)
        panel.append(self.lbl_hint)

        spacer = Gtk.Box()
        spacer.set_vexpand(True)
        panel.append(spacer)

        self.lbl_sys = Gtk.Label(label="", xalign=0)
        self.lbl_sys.add_css_class("small")
        self.lbl_sys.set_wrap(True)
        panel.append(self.lbl_sys)

        root.append(ov)
        root.append(panel)
        self.set_child(root)

    def _sep(self):
        s = Gtk.Box()
        s.add_css_class("sep")
        s.set_size_request(-1, 1)
        return s

    def _metric(self, grid, row, key):
        k = Gtk.Label(label=key, xalign=0)
        k.add_css_class("metric-key")
        v = Gtk.Label(label="0:00", xalign=0)
        v.add_css_class("metric-val")
        grid.attach(k, 0, row, 1, 1)
        grid.attach(v, 1, row, 1, 1)
        return v

    def _install_keys(self):
        kc = Gtk.EventControllerKey()

        def on_key(_c, keyval, _code, _mods):
            name = Gdk.keyval_name(keyval) or ""
            if name in ("Escape", "q", "Q"):
                now = time.time()
                if now - self._quit_armed < 2.0:
                    try:
                        open("/tmp/.posture-monitor.quit", "w").close()   # 让启动器不要再拉起
                    except Exception:
                        pass
                    self.get_application().quit()
                else:
                    self._quit_armed = now
                    self.flash("再按一次 Esc 退出演示程序", "窗口会自动重启，或用 scripts/uninstall.sh 移除")
                return True
            if name in ("f", "F"):
                self.stats.event("hotkey", "toggle_fullscreen")
                if self.is_fullscreen():
                    self.unfullscreen()
                else:
                    self.fullscreen()
                return True
            if name in ("r", "R"):
                # 手工触发一次护眼休息，演示用
                self.start_rest(float(self.cfg.eye_rest_duration_s))
                return True
            return False

        kc.connect("key-pressed", on_key)
        self.add_controller(kc)

    # ---------------- 画面 ----------------
    def update_frame(self, frame, seq, pose, map_info):
        self.frame = frame
        self.frame_seq = seq
        self.pose = pose
        self.map_info = map_info
        now = time.time()
        self._frame_times.append(now)
        self._frame_times = [t for t in self._frame_times if now - t < 2.0]
        if len(self._frame_times) > 1:
            self.fps = (len(self._frame_times) - 1) / (self._frame_times[-1] - self._frame_times[0])

    def _on_draw(self, area, ctx, width, height):
        ctx.set_source_rgb(0.02, 0.03, 0.05)
        ctx.paint()
        if self.frame is None:
            self._centered_text(ctx, width, height, "正在打开摄像头…", 20)
            return
        surf = self.surface.update(self.frame)
        fh, fw = self.frame.shape[:2]
        scale = min(width / fw, height / fh)
        dw, dh = fw * scale, fh * scale
        ox, oy = (width - dw) / 2.0, (height - dh) / 2.0
        ctx.save()
        ctx.translate(ox, oy)
        ctx.scale(scale, scale)
        ctx.set_source_surface(surf, 0, 0)
        ctx.get_source().set_filter(1)      # 1 = bilinear
        ctx.paint()
        ctx.restore()

        pose = self.pose
        if pose is not None and len(pose) and self.map_info:
            r, padx, pady = self.map_info
            best = self._main_index(pose)

            def to_widget(x, y):
                fx = (x - padx) / r
                fy = (y - pady) / r
                return (ox + fx * scale, oy + fy * scale)

            bad = self.engine.st.bad
            overlay.draw_box(ctx, pose.boxes[best], to_widget,
                             (1.0, 0.35, 0.35) if bad else (0.30, 0.85, 0.45),
                             f"person {pose.scores[best]:.2f}")
            overlay.draw_skeleton(ctx, pose.kpts[best], to_widget, bad=bad,
                                  show_names=bool(self.cfg.show_names))

        # 左下角 HUD
        ctx.save()
        ctx.select_font_face("Sans", 0, 0)
        ctx.set_font_size(12)
        ctx.set_source_rgba(0, 0, 0, 0.45)
        ctx.rectangle(8, height - 30, 380, 22)
        ctx.fill()
        ctx.set_source_rgba(0.75, 0.85, 1.0, 0.9)
        lat = f"{pose.latency_ms:.0f}ms" if pose is not None else "--"
        ctx.move_to(14, height - 14)
        ctx.show_text(f"{self.backend_desc} {lat} | 采集 {self.fps:.1f}fps | "
                      f"{'有人员' if self.engine.st.present else '无人员'}")
        ctx.restore()

    @staticmethod
    def _main_index(pose) -> int:
        """选主人物：画面里面积最大的那个（一体机场景只有一个人）。"""
        areas = pose.boxes[:, 2] * pose.boxes[:, 3]
        return int(areas.argmax())

    def _centered_text(self, ctx, w, h, text, size):
        ctx.select_font_face("Sans", 0, 1)
        ctx.set_font_size(size)
        ext = ctx.text_extents(text)
        ctx.set_source_rgb(0.75, 0.82, 0.9)
        ctx.move_to((w - ext.width) / 2, h / 2)
        ctx.show_text(text)

    def _tick_ui(self):
        self.area.queue_draw()
        if self.rest_until and time.time() > self.rest_until:
            self.rest_until = 0.0
            self.rest_box.set_visible(False)
            self.stats.event("eye_rest", "done")
        return True

    # ---------------- 面板 ----------------
    def _tick_panel(self):
        now = time.time()
        st = self.engine.st
        m = st.metrics

        if not st.present:
            self.lbl_state.set_text("等待入座")
            self.lbl_state.remove_css_class("state-bad")
            self.lbl_state.remove_css_class("state-ok")
            self.lbl_state.add_css_class("state-idle")
            self.lbl_reason.set_text("请坐到摄像头前")
        elif st.bad:
            self.lbl_state.set_text(self.engine.main_label())
            self.lbl_state.remove_css_class("state-ok")
            self.lbl_state.remove_css_class("state-idle")
            self.lbl_state.add_css_class("state-bad")
            self.lbl_reason.set_text(
                f"低头 {m.head_drop:.2f} · 肩倾 {m.shoulder_tilt_deg:.1f}° · "
                f"头倾 {m.head_tilt_deg:.1f}° · 手脸 {m.hand_to_face:.2f}")
        else:
            self.lbl_state.set_text("坐姿正常")
            self.lbl_state.remove_css_class("state-bad")
            self.lbl_state.remove_css_class("state-idle")
            self.lbl_state.add_css_class("state-ok")
            self.lbl_reason.set_text(
                f"低头 {m.head_drop:.2f} · 肩倾 {m.shoulder_tilt_deg:.1f}° · "
                f"头倾 {m.head_tilt_deg:.1f}° · 前倾 {m.lean:.2f}")

        self.lbl_cont.set_text(fmt_dur(self.engine.bad_duration(now)))
        self.lbl_cont.remove_css_class("metric-val" if not st.bad else "metric-val-bad")
        self.lbl_cont.add_css_class("metric-val-bad" if st.bad else "metric-val")

        today = self.stats.today()
        # 今日不良 = 持久化累计 + 本次会话未落盘的增量
        total_bad = today["bad_s"] + self.engine.total_bad_s
        self.lbl_today.set_text(fmt_dur(total_bad))

        if st.present:
            if not self.seated_since:
                self.seated_since = st.present_since or now
            self.lbl_seated.set_text(fmt_dur(now - self.seated_since))
        else:
            self.seated_since = 0.0
            self.lbl_seated.set_text("0:00")

        counts = dict(today["counts"])
        for k, v in self.engine.counts().items():
            counts[k] = counts.get(k, 0) + v
        for key, lbl in self.count_labels.items():
            lbl.set_text(str(counts.get(key, 0)))

        remain = max(0.0, self.next_eye_rest - now)
        self.lbl_eye_val.set_text(fmt_dur(remain) if remain else "即将提醒")

        # ---- 提醒判定 ----
        bad_dur = self.engine.bad_duration(now)
        if (st.bad and bad_dur >= float(self.cfg.bad_posture_remind_s)
                and now - self.last_remind >= float(self.cfg.remind_cooldown_s)):
            self.trigger_posture_alert(bad_dur)
        if now >= self.next_eye_rest and not self.rest_until:
            self.start_rest(float(self.cfg.eye_rest_duration_s))
            self.next_eye_rest = now + float(self.cfg.eye_rest_interval_s)
        if (st.present and self.seated_since
                and now - self.seated_since >= float(self.cfg.sedentary_interval_s)
                and now >= self.next_sedentary):
            self.flash("久坐提醒 · 起来活动 2 分钟",
                       f"已连续保持坐姿 {fmt_dur(now - self.seated_since)}", 12.0)
            self.stats.event("sedentary", f"{now - self.seated_since:.0f}s")
            self.next_sedentary = now + float(self.cfg.sedentary_interval_s)

        if self.alert_until and now > self.alert_until:
            self.alert_until = 0.0
            self.banner.set_visible(False)

        self.lbl_hint.set_text(
            "快捷键：R 手动护眼 · F 切换全屏 · Esc×2 退出"
            if self.cfg.fullscreen else "")
        self.lbl_sys.set_text(
            f"后端 {self.backend_desc}\n{self.sdk}\n"
            f"推理 {self.pose.latency_ms:.0f} ms · 显示 {self.fps:.1f} fps"
            if self.pose is not None else f"后端 {self.backend_desc}\n{self.sdk}")

        self.stats.add(0.5 if st.present else 0.0, 0.5 if st.bad else 0.0)
        self._write_status(st, counts, total_bad)
        return True

    def _write_status(self, st, counts, total_bad):
        """写 /run/posture-monitor/status.json：给运维/自检一个无需看屏的可观测点。"""
        try:
            w = self.get_width()
            h = self.get_height()
            data = {
                "ts": round(time.time(), 1),
                "window": [w, h],
                "fullscreen": bool(self.is_fullscreen()),
                "backend": self.backend_desc,
                "sdk": self.sdk,
                "latency_ms": round(self.pose.latency_ms, 1) if self.pose is not None else None,
                "persons": len(self.pose) if self.pose is not None else 0,
                "fps": round(self.fps, 1),
                "present": st.present,
                "bad": st.bad,
                "label": self.engine.main_label(),
                "bad_duration_s": round(self.engine.bad_duration(), 1),
                "today_bad_s": round(total_bad, 1),
                "counts": counts,
                "rest_active": bool(self.rest_until),
                "banner": self.alert_text if self.alert_until else "",
            }
            path = "/run/posture-monitor/status.json"
            os.makedirs(os.path.dirname(path), exist_ok=True)
            tmp = path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(data, f, ensure_ascii=False)
            os.replace(tmp, path)
        except Exception:
            pass

    # ---------------- 提醒 ----------------
    def trigger_posture_alert(self, bad_dur):
        now = time.time()
        self.last_remind = now
        self.alert_until = now + 8.0
        self.flash(f"坐姿提醒 · {self.engine.main_label()}",
                   f"已经连续 {fmt_dur(bad_dur)} 坐姿不良，请坐正并放松肩膀", 8.0)
        self.stats.event("posture_alert", self.engine.main_label())
        beep()
        # 记一次违规时长
        for key, r in self.engine.rules.items():
            if r.on:
                self.stats.violation_start(key, r.since)
                self.stats.violation_end(key, now)
                r.since = now

    def flash(self, text, sub="", seconds=5.0):
        self.banner_text.set_text(text)
        self.banner_sub.set_text(sub)
        self.banner.set_visible(True)
        self.alert_until = time.time() + seconds
        self.alert_text, self.alert_sub = text, sub

    def start_rest(self, seconds):
        self.rest_until = time.time() + seconds
        self.rest_box.set_visible(True)
        self.stats.event("eye_rest", f"start {seconds:.0f}s")
        beep(double=True)

        def countdown():
            if not self.rest_until:
                return False
            left = int(math.ceil(self.rest_until - time.time()))
            self.rest_count.set_text(str(max(0, left)))
            return True

        GLib.timeout_add(250, countdown)


def beep(double=False):
    """用系统里的 paplay/pw-play 播一个短提示音（无则静默）。"""
    wav = "/opt/posture-monitor/assets/beep.wav"
    players = ("paplay", "pw-play", "aplay")
    for p in players:
        try:
            if double:
                subprocess.Popen([p, wav], stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL)
                GLib.timeout_add(260, lambda: (subprocess.Popen(
                    [p, wav], stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL), False)[1])
            else:
                subprocess.Popen([p, wav], stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL)
            return p
        except FileNotFoundError:
            continue
        except Exception:
            continue
    return None
