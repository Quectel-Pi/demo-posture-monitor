#!/usr/bin/env python3
"""只读采样：把一帧 status 里的判定相关字段落成 JSONL。

为什么单独写：socket 上 status / video / notification 三类消息混流，
video 帧带 JPEG base64（巨大），必须丢掉只留 status。
本脚本只 connect + read，不写板上任何文件，不影响生产。
"""
import json, socket, sys, time

FIELDS = ("posture_state","explanation","present","persons","frame_id",
          "metrics_valid","shoulder_angle_deg","head_offset_y","head_axis_deg",
          "ear_angle_deg","ear_angle_valid","width_ratio","width_ratio_raw",
          "wrist_to_nose","wrist_to_nose_raw","wrist_y_rel","wrist_valid",
          "wrist_above_shoulder","ear_mid_offset_y","ear_mid_valid",
          "eye_mid_offset_y","eye_mid_valid","reject_reason","profile_active",
          "profile_label","profile_rule","profile_strength","has_baseline",
          "baseline_created_at","calibrating","keypoints_valid","box_valid")

def main():
    out = open(sys.argv[1], "w")
    dur = float(sys.argv[2]) if len(sys.argv) > 2 else 600
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(10.0); s.connect("/run/posture-guard/status.sock")
    buf = b""; t0 = time.time(); n = 0
    while time.time() - t0 < dur:
        try: chunk = s.recv(300000)
        except socket.timeout: break
        if not chunk: break
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            if b'"status"' not in line: continue
            try: m = json.loads(line)
            except ValueError: continue
            if m.get("type") != "status": continue
            rec = {k: m.get(k) for k in FIELDS}
            rec["t"] = round(time.time() - t0, 3)
            kp = m.get("keypoints") or []
            rec["kp"] = kp if m.get("keypoints_valid") and len(kp) >= 34 else None
            out.write(json.dumps(rec, ensure_ascii=False) + "\n"); n += 1
    out.close(); print("frames", n, "in", round(time.time() - t0, 1), "s", file=sys.stderr)

main()
