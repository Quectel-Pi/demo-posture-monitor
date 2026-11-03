#!/usr/bin/env bash
# 板端：对带标签的姿势图逐个跑关键点提取，输出 jsonl
set -uo pipefail
OUT=/tmp/kp_export.jsonl
: > "$OUT"
cd /tmp
for d in normal head_down head_tilt lean_close hand_face away; do
  for f in /var/lib/posture-guard/poses/$d/*.jpg; do
    [ -f "$f" ] || continue
    line=$(sudo /opt/posture-guard/bin/posture_guard \
       --config /etc/posture-guard/posture.yaml --image "$f" --no-socket 2>/dev/null \
       | grep -E '^\{"image"' | tail -1)
    if [ -n "$line" ]; then
      echo "{\"pose\":\"$d\",\"data\":$line}" >> "$OUT"
    else
      echo "{\"pose\":\"$d\",\"data\":null}" >> "$OUT"
    fi
  done
done
wc -l "$OUT"
