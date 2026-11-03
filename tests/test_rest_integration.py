#!/usr/bin/env python3
"""Board-only accelerated NPU -> eye timer -> socket -> offline audio check.

Uses a temporary configuration/state directory and a static supplied sample image.
Does not change the running service or its real 20-minute threshold.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time

ap = argparse.ArgumentParser()
ap.add_argument('--binary', default='/opt/posture-guard/bin/posture_guard')
ap.add_argument('--image', required=True)
ap.add_argument('--audio', action='store_true')
args = ap.parse_args()
with tempfile.TemporaryDirectory(prefix='posture-rest-test-') as temp:
    root = Path(temp)
    config = Path('/etc/posture-guard/posture.yaml').read_text()
    config = config.replace('/run/posture-guard/status.sock', str(root / 'status.sock'))
    config = config.replace('/var/lib/posture-guard', str(root / 'state'))
    config = config.replace('stream_video: true', 'stream_video: false')
    (root / 'test.yaml').write_text(config)
    with (root / 'service.log').open('w+') as log:
        daemon = subprocess.Popen([args.binary, '--config', str(root / 'test.yaml'),
                                   '--source-image', args.image, '--eye-seconds', '3'],
                                  stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 20
            while not (root / 'status.sock').exists():
                if daemon.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('test service failed to start')
                time.sleep(.1)
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.settimeout(15)
                client.connect(str(root / 'status.sock'))
                data = b''
                found = False
                while time.monotonic() < deadline and not found:
                    data += client.recv(65536)
                    while b'\n' in data:
                        line, data = data.split(b'\n', 1)
                        msg = json.loads(line)
                        if msg.get('type') == 'notification' and msg.get('key') == 'rest':
                            found = True
                            print('PASS: NPU sample -> presence -> accelerated 3s eye timer -> rest event')
                            if args.audio:
                                spec = importlib.util.spec_from_file_location('ui', Path(__file__).resolve().parents[1] / 'ui/posture_guard_ui.py')
                                ui = importlib.util.module_from_spec(spec)
                                spec.loader.exec_module(ui)
                                voice = ui.VoicePlayer()
                                if not voice.play('rest'):
                                    raise RuntimeError('voice playback did not start')
                                if voice.process.wait(timeout=20) != 0:
                                    raise RuntimeError('voice playback failed')
                                print('PASS: rest event -> local Mandarin WAV -> desktop paplay exit 0')
                            break
                if not found:
                    raise RuntimeError('no rest event')
        finally:
            daemon.terminate()
            try:
                daemon.wait(timeout=5)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.wait()
            if daemon.returncode not in (0, -15):
                log.seek(0)
                print(log.read()[-4000:])
