"""GTK4 应用入口。"""
from __future__ import annotations

import signal
import sys
import time

import gi

gi.require_version("Gtk", "4.0")
from gi.repository import Gtk  # noqa: E402

from . import config as cfgmod  # noqa: E402
from .pipeline import Pipeline  # noqa: E402
from .stats import Stats  # noqa: E402
from .window import PostureWindow  # noqa: E402


class PostureApp(Gtk.Application):
    def __init__(self, cfg, pipeline, stats):
        super().__init__(application_id="com.quectel.PostureMonitor")
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
    pipe = Pipeline(cfg, source=source).start()

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
