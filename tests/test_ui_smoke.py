#!/usr/bin/env python3
"""Run on the board in its GTK desktop session; no calibration commands are sent."""
import importlib.util
from pathlib import Path
import sys
import time

spec = importlib.util.spec_from_file_location('posture_ui', Path(__file__).resolve().parents[1] / 'ui/posture_guard_ui.py')
ui = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ui)
from gi.repository import GLib
app, state = ui.build_ui(True, '/run/posture-guard/status.sock')
failed = []

def run_checks():
    try:
        win = app.get_active_window()
        assert win is not None
        sounds = []
        def fake_play(key, **kwargs):
            sounds.append(key)
            return True
        win.voice.play = fake_play
        msg = dict(posture_state='NORMAL', posture_state_zh='坐姿良好',
                   session_state='ACTIVE', voice_enabled=True, use_s=300,
                   remaining_s=900, has_baseline=True, fps=15, latency_ms=19)
        win._apply_status(msg)
        assert win.lbl_state.get_text() == '坐姿良好'
        assert abs(win.eye_progress.get_fraction() - .25) < .01
        win._apply_event(dict(key='head_down', title='请抬头', detail='持续低头'))
        assert sounds == ['head_down']
        before=len(sounds)
        retry=dict(msg,posture_state='HEAD_DOWN',posture_state_zh='低头',
                   last_notification_key='head_down',last_notification_id='reconnect-test',
                   last_notification_ago_s=1,bad_posture_frames=10,posture_reminder_frames=10,
                   posture_reminder_reason='notified')
        win._apply_status(dict(retry,bad_posture_frames=9))
        assert len(sounds)==before  # reconnect fallback never bypasses the tenth frame
        win._apply_status(retry)
        win._apply_status(retry)
        win._apply_status(dict(retry,posture_reminder_reason="cooldown",posture_reminder_cooldown_s=42.3))
        assert "冷却剩余 43 秒" in win.lbl_info.get_text()
        assert len(sounds)==before+1  # reconnect retry is delivered once
        win._apply_status(dict(msg,last_notification_key='head_down',
                               last_notification_id='stale-event',last_notification_ago_s=1))
        assert len(sounds)==before+1  # corrected posture never replays an old event
        win._apply_status(dict(msg, session_state='BREAK_DUE', rest_remaining_s=3))
        win._tick()
        assert win.rest_box.get_visible() and not win.btn_ack.get_sensitive()
        assert sounds[-1] == 'rest'
        state['rest_deadline'] = time.monotonic() - 1
        win._tick()
        assert win.btn_ack.get_sensitive()
        win._apply_status(msg)
        assert not win.rest_box.get_visible()
        win._toggle_voice()
        assert not win.voice.enabled
        win._apply_status(dict(msg, calibrating=True, calibration_progress=50))
        assert win.lbl_state.get_text() == '校准中'
        win._apply_status(msg)
        assert not win.lbl_state.has_css_class('state-cal')
        win._apply_status(dict(msg,posture_state='UNKNOWN',posture_state_zh='无法判定',explanation='画像判定中…'))
        assert win.lbl_state.get_text() == '正在确认坐姿'
        win._apply_status(msg)
        print('PASS: GTK status, progress, voice routing, rest countdown, mute, calibration recovery')
    except Exception as exc:
        failed.append(str(exc))
        import traceback
        traceback.print_exc()
    finally:
        app.quit()
    return False

def activated(*_):
    GLib.timeout_add(400, run_checks)
app.connect('activate', activated)
app.run([])
sys.exit(bool(failed))
