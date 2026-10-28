"""GTK4 应用入口。"""
from __future__ import annotations

import signal
import sys
import time

import gi

gi.require_version("Gtk", "4.0")
from gi.repository import Gio, Gtk  # noqa: E402

from . import config as cfgmod  # noqa: E402
from .pipeline import Pipeline  # noqa: E402
from .stats import Stats  # noqa: E402
from .window import PostureWindow  # noqa: E402


class PostureApp(Gtk.Application):
    def __init__(self, cfg, pipeline, stats):
        # NON_UNIQUE：一体机上只有一个实例，也不需要单实例转发。
        # 真正的原因是从 autostart 拉起时用户 D-Bus 有时还没就绪，GtkApplication
        # 会打一条 Gtk-CRITICAL "Unable to register the application ... NoReply"，
        # 主流程无害但日志噪声大 —— 关掉 D-Bus 注册即可。
        super().__init__(application_id="com.quectel.PostureMonitor",
                         flags=Gio.ApplicationFlags.NON_UNIQUE)
        self.cfg = cfg
        self.pipeline = pipeline
        self.stats = stats
        self.win = None

    def do_activate(self):
        if self.win is None:
            self.win = PostureWindow(self, self.cfg, self.pipeline.backend,
                                     self.pipeline.engine, self.stats,
                                     self.pipeline.backend_desc, self.pipeline.sdk)
            GLibSync.start(self.win, self.pipeline)
        self.win.present()

    def do_shutdown(self):
        try:
            self.pipeline.stop()
            self.stats.close()
        finally:
            Gtk.Application.do_shutdown(self)


class GLibSync:
    """把 pipeline 的最新结果推给窗口（GLib 主循环里跑）。"""

    @staticmethod
    def start(win, pipeline):
        from gi.repository import GLib

        def tick():
            frame, seq, pose, map_info = pipeline.latest()
            if frame is not None:
                win.update_frame(frame, seq, pose, map_info)
            return True

        GLib.timeout_add(33, tick)


def main(argv=None) -> int:
    argv = list(sys.argv if argv is None else argv)
    overrides = {}
    source = None
    it = iter(argv[1:])
    for a in it:
        if a.startswith("--source="):
            source = a.split("=", 1)[1]
        elif a == "--source":
            source = next(it, None)
        elif a.startswith("--"):
            k, _, v = a[2:].partition("=")
            if not _:
                continue
            try:
                overrides[k] = int(v) if v.lstrip("-").isdigit() else (
                    float(v) if _is_float(v) else (v.lower() in ("1", "true", "yes")))
            except Exception:
                overrides[k] = v

    cfg = cfgmod.Config.load(overrides)
    stats = Stats(cfg.db_path)
    pipe = Pipeline(cfg, source=source, stats=stats).start()

    app = PostureApp(cfg, pipe, stats)

    def _sig(*_a):
        pipe.stop()
        stats.close()
        Gtk.Application.quit(app)

    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, _sig)
    return app.run([argv[0]])


def _is_float(v: str) -> bool:
    try:
        float(v)
        return True
    except ValueError:
        return False


if __name__ == "__main__":
    raise SystemExit(main())
