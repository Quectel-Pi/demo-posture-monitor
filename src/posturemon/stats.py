"""统计持久化：SQLite（板端 sqlite3 为 python 标准库，无需额外安装）。"""
from __future__ import annotations

import os
import sqlite3
import threading
import time
from datetime import date, datetime

SCHEMA = """
CREATE TABLE IF NOT EXISTS day (
    day        TEXT PRIMARY KEY,
    present_s  REAL NOT NULL DEFAULT 0,
    bad_s      REAL NOT NULL DEFAULT 0,
    frames     INTEGER NOT NULL DEFAULT 0,
    updated    REAL NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS violation (
    id       INTEGER PRIMARY KEY AUTOINCREMENT,
    day      TEXT NOT NULL,
    ts       REAL NOT NULL,
    kind     TEXT NOT NULL,
    duration REAL NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_violation_day ON violation(day);
CREATE TABLE IF NOT EXISTS event (
    id    INTEGER PRIMARY KEY AUTOINCREMENT,
    ts    REAL NOT NULL,
    kind  TEXT NOT NULL,
    note  TEXT
);
"""


class Stats:
    def __init__(self, db_path: str, flush_s: float = 10.0):
        self.db_path = db_path
        self.flush_s = flush_s
        # RLock：today()/days() 会在持锁状态下调用 flush()，用普通 Lock 会自锁（踩过）
        self._lock = threading.RLock()
        os.makedirs(os.path.dirname(db_path), exist_ok=True)
        self._conn = sqlite3.connect(db_path, check_same_thread=False)
        self._conn.execute("PRAGMA journal_mode=WAL")
        self._conn.executescript(SCHEMA)
        self._conn.commit()
        self._pending = {"present_s": 0.0, "bad_s": 0.0, "frames": 0}
        self._last_flush = time.time()
        self._open_violation: dict[str, float] = {}

    # ---------------- 累加 ----------------
    def add(self, present_s: float, bad_s: float, frames: int = 0):
        with self._lock:
            self._pending["present_s"] += present_s
            self._pending["bad_s"] += bad_s
            self._pending["frames"] += frames
        if time.time() - self._last_flush >= self.flush_s:
            self.flush()

    def flush(self):
        with self._lock:
            p = self._pending
            if p["present_s"] or p["bad_s"] or p["frames"]:
                today = date.today().isoformat()
                self._conn.execute(
                    """INSERT INTO day(day, present_s, bad_s, frames, updated)
                       VALUES(?,?,?,?,?)
                       ON CONFLICT(day) DO UPDATE SET
                         present_s = present_s + excluded.present_s,
                         bad_s     = bad_s + excluded.bad_s,
                         frames    = frames + excluded.frames,
                         updated   = excluded.updated""",
                    (today, p["present_s"], p["bad_s"], p["frames"], time.time()))
                self._conn.commit()
                p["present_s"] = p["bad_s"] = 0.0
                p["frames"] = 0
            self._last_flush = time.time()

    # ---------------- 违规事件 ----------------
    def violation_start(self, kind: str, ts: float | None = None):
        with self._lock:
            self._open_violation.setdefault(kind, ts if ts is not None else time.time())

    def violation_end(self, kind: str, ts: float | None = None):
        now = ts if ts is not None else time.time()
        with self._lock:
            start = self._open_violation.pop(kind, None)
            if start is None:
                return
            self._conn.execute(
                "INSERT INTO violation(day, ts, kind, duration) VALUES(?,?,?,?)",
                (date.fromtimestamp(start).isoformat(), start, kind, max(0.0, now - start)))
            self._conn.commit()

    def event(self, kind: str, note: str = ""):
        with self._lock:
            self._conn.execute("INSERT INTO event(ts, kind, note) VALUES(?,?,?)",
                               (time.time(), kind, note))
            self._conn.commit()

    # ---------------- 查询 ----------------
    def today(self) -> dict:
        with self._lock:
            self.flush()
            row = self._conn.execute(
                "SELECT present_s, bad_s, frames FROM day WHERE day=?",
                (date.today().isoformat(),)).fetchone() or (0, 0, 0)
            counts = dict(self._conn.execute(
                "SELECT kind, COUNT(*) FROM violation WHERE day=? GROUP BY kind",
                (date.today().isoformat(),)).fetchall())
        return {"present_s": row[0] + self._pending["present_s"],
                "bad_s": row[1] + self._pending["bad_s"],
                "frames": row[2] + self._pending["frames"],
                "counts": counts}

    def days(self, n: int = 7) -> list[tuple]:
        with self._lock:
            self.flush()
            return self._conn.execute(
                "SELECT day, present_s, bad_s FROM day ORDER BY day DESC LIMIT ?",
                (n,)).fetchall()

    def close(self):
        try:
            self.flush()
            self._conn.close()
        except Exception:
            pass


def fmt_dur(sec: float) -> str:
    sec = int(max(0, sec))
    h, m, s = sec // 3600, (sec % 3600) // 60, sec % 60
    if h:
        return f"{h}:{m:02d}:{s:02d}"
    return f"{m:02d}:{s:02d}"


def fmt_clock(ts: float) -> str:
    return datetime.fromtimestamp(ts).strftime("%H:%M:%S")
