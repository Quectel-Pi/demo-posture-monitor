"""Voice playback is bounded, interruptible, and restricted to fixed prompts."""
import importlib.util
import math
import struct
import wave
from pathlib import Path
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('posture_ui', ROOT / 'ui/posture_guard_ui.py')
ui = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ui)

# 与 ui/posture_guard_ui.py 的 VoicePlayer.play 白名单保持一致
VOICE_KEYS = ('shoulder_tilt', 'head_down', 'head_tilt',
              'too_close', 'hand_on_face', 'rest')
VOICE_DIR = ROOT / 'assets' / 'voice'
SAMPLE_RATE = 24000
# 板端 paplay 直接放 WAV，素材必须是 24kHz mono 16bit；
# 峰值留余量（不削波），RMS 有下限（不然「喇叭在响但听不清」）。
MIN_PEAK_PCT, MAX_PEAK_PCT = 5.0, 97.0
MIN_RMS_PCT = 15.0


def read_wav(path):
    with wave.open(str(path), 'rb') as w:
        params = {
            'channels': w.getnchannels(),
            'width': w.getsampwidth(),
            'rate': w.getframerate(),
            'frames': w.getnframes(),
        }
        raw = w.readframes(params['frames'])
    vals = struct.unpack('<%dh' % (len(raw) // 2), raw) if raw else ()
    peak = max((abs(v) for v in vals), default=0)
    rms = math.sqrt(sum(v * v for v in vals) / len(vals)) if vals else 0.0
    return params, peak / 32767 * 100, rms / 327.67


class VoiceTests(unittest.TestCase):
    @patch.object(ui.subprocess, 'Popen')
    def test_bounded_playback(self, spawn):
        proc = Mock()
        proc.poll.return_value = None
        spawn.return_value = proc
        player = ui.VoicePlayer()
        self.assertTrue(player.play('head_down'))
        self.assertFalse(player.play('head_tilt'))
        self.assertTrue(player.play('rest'))
        proc.terminate.assert_called_once()
        self.assertFalse(player.play('rest'))  # event and status arrive together
        self.assertEqual(spawn.call_count, 2)
        player.enabled = False
        self.assertFalse(player.play('rest', force=True))
        self.assertFalse(player.play('../../some-command', force=True))
        player.stop()

    @patch.object(ui.subprocess, 'Popen', side_effect=FileNotFoundError('paplay'))
    def test_missing_player(self, spawn):
        player = ui.VoicePlayer()
        self.assertFalse(player.play('head_down'))
        self.assertIn('paplay', player.error)


class VoiceDeliveryTests(unittest.TestCase):
    @patch.object(ui.subprocess, 'Popen')
    @patch.object(ui.time, 'monotonic')
    def test_busy_prompt_is_queued_and_deduplicated(self, clock, spawn):
        clock.return_value = 100
        proc = Mock(); proc.poll.return_value = None; spawn.return_value = proc
        p = ui.VoicePlayer()
        try:
            self.assertTrue(p.request('head_down', 'event-1'))
            clock.return_value = 100.5
            self.assertFalse(p.request('head_tilt', 'event-2'))
            self.assertEqual(p.pending[0], 'head_tilt')
            clock.return_value = 102.1
            self.assertTrue(p.tick(['head_tilt']))
            self.assertIsNone(p.pending)
            self.assertFalse(p.request('head_tilt', 'event-2'))
            self.assertEqual(spawn.call_count, 2)
        finally: p.stop()

    @patch.object(ui.subprocess, 'Popen')
    @patch.object(ui.time, 'monotonic')
    def test_corrected_pose_discards_stale_pending_voice(self, clock, spawn):
        clock.return_value = 100
        proc = Mock(); proc.poll.return_value = None; spawn.return_value = proc
        p = ui.VoicePlayer()
        try:
            p.request('head_down', 'event-1')
            clock.return_value = 100.5
            p.request('head_tilt', 'event-2')
            clock.return_value = 103
            self.assertFalse(p.tick([]))
            self.assertIsNone(p.pending)
            self.assertEqual(spawn.call_count, 1)
        finally: p.stop()

    @patch.object(ui.subprocess, 'Popen')
    @patch.object(ui.time, 'monotonic')
    def test_unplayed_event_can_recover_after_pending_is_discarded(self, clock, spawn):
        clock.return_value=100
        proc=Mock();proc.poll.return_value=None;spawn.return_value=proc
        p=ui.VoicePlayer()
        try:
            p.request('head_down','first')
            clock.return_value=100.5
            p.request('head_tilt','unplayed')
            self.assertNotIn('unplayed',p.seen_ids)
            p.tick([])  # transient UNKNOWN/correction discards the pending prompt
            clock.return_value=102.5
            self.assertTrue(p.request('head_tilt','unplayed'))
            self.assertIn('unplayed',p.seen_ids)
            self.assertFalse(p.request('head_tilt','unplayed'))
            self.assertEqual(spawn.call_count,2)
        finally:p.stop()

    @patch.object(ui.subprocess, 'Popen')
    def test_player_exit_failure_is_observable(self, spawn):
        proc = Mock(); proc.poll.return_value = None; spawn.return_value = proc
        p = ui.VoicePlayer()
        try:
            p.request('head_down', 'event-1')
            p.stderr_file.write(b'Connection refused')
            proc.poll.return_value = 1
            p.poll()
            self.assertIn('Connection refused', p.error)
            self.assertIsNotNone(p.pending)
        finally: p.stop()


class VoiceAssetTests(unittest.TestCase):
    """静态检查 assets/voice/ 下的素材：存在、格式正确、不削波、不空。

    不跑合成、不联网、不需要 edge-tts/espeak，所以在开发和 CI 都能跑。
    """

    def test_all_prompts_present_and_well_formed(self):
        for key in VOICE_KEYS:
            with self.subTest(key=key):
                path = VOICE_DIR / (key + '.wav')
                self.assertTrue(path.is_file(), f'{path} 缺失')
                self.assertGreater(path.stat().st_size, 1024, f'{path} 太小')
                params, peak, rms = read_wav(path)
                self.assertEqual(params['channels'], 1, f'{key}: 必须单声道')
                self.assertEqual(params['width'], 2, f'{key}: 必须 16bit')
                self.assertEqual(params['rate'], SAMPLE_RATE, f'{key}: 必须 24kHz')
                seconds = params['frames'] / params['rate']
                self.assertGreater(seconds, 1.0, f'{key}: 太短 {seconds:.2f}s')
                self.assertLess(seconds, 10.0, f'{key}: 太长 {seconds:.2f}s')
                self.assertGreaterEqual(peak, MIN_PEAK_PCT,
                                        f'{key}: 峰值只有 {peak:.1f}%，素材几乎是静音')
                self.assertLessEqual(peak, MAX_PEAK_PCT,
                                     f'{key}: 峰值 {peak:.1f}%，已经削波')
                self.assertGreaterEqual(rms, MIN_RMS_PCT,
                                        f'{key}: RMS 只有 {rms:.1f}%，听不清')

    def test_no_orphan_wavs(self):
        """assets/voice/ 里不该有白名单之外、或者根本没被引用的 wav。"""
        on_disk = {p.stem for p in VOICE_DIR.glob('*.wav')}
        self.assertEqual(on_disk, set(VOICE_KEYS))


if __name__ == '__main__':
    unittest.main()
