#!/usr/bin/env python3
"""实测校验 —— 摆一个姿势，看系统判成什么（端到端，走真实服务）

用法（板端）：
    python3 scripts/live_check.py --expect normal --seconds 10 --settle 5
"""
from __future__ import annotations
import argparse, importlib.util, json, os, socket, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("tp", os.path.join(HERE, "teach_posture.py"))
tp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tp)

ZH = {"normal": "正常", "head_down": "低头", "head_tilt": "歪头",
      "too_close": "离屏太近", "hand_face": "托腮"}
STATE_MAP = {"NORMAL": "normal", "HEAD_DOWN": "head_down", "HEAD_TILT": "head_tilt",
             "TOO_CLOSE": "too_close", "HAND_ON_FACE": "hand_face", "UNKNOWN": "?"}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--expect", required=True)
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--settle", type=float, default=5.0)
    a = ap.parse_args()

    print(f"== 实测「{ZH.get(a.expect, a.expect)}」==")
    print(f"   请摆好该姿势，{a.settle:.0f} 秒后开始采样（采样 {a.seconds:.0f} 秒）")
    for i in range(int(a.settle), 0, -1):
        print(f"   {i}…", end="\r", flush=True)
        time.sleep(1)

    end = time.time() + a.seconds
    states, states_inst, rows = [], [], []
    while time.time() < end:
        st = tp.read_status()
        if st is None:
            time.sleep(0.2)
            continue
        got = STATE_MAP.get(st.get("posture_state", ""), "?")
        states.append(got)
        inst = st.get("profile_label") or ""
        if inst:
            states_inst.append(inst)
        rows.append((got, st.get("profile_rule") or st.get("explanation") or "",
                     st.get("profile_strength") or 0.0))
        time.sleep(0.2)

    if not states:
        print("!! 读不到状态")
        return 1
    from collections import Counter
    cnt = Counter(states)
    hit = cnt.get(a.expect, 0)
    print(f"\n   采样 {len(states)} 帧，命中「{ZH.get(a.expect, a.expect)}」{hit} 帧 "
          f"= {hit/len(states)*100:.0f}%")
    for k, v in cnt.most_common():
        print(f"     {k:12s} {v:3d} 帧 = {v/len(states)*100:5.1f}%")
    if states_inst:
        cnt2 = Counter(states_inst)
        hit2 = cnt2.get(a.expect, 0)
        print(f"   瞬时分类（不含 2s 保持）：命中 {hit2}/{len(states_inst)} = "
              f"{hit2/len(states_inst)*100:.0f}%  " +
              " ".join(f"{k}:{v}" for k, v in cnt2.most_common()))
    # 打印几帧典型原因
    for got, reason, strength in rows[:1] + rows[len(rows)//2:len(rows)//2+1]:
        print(f"     例：{got:12s} 强度 {strength:+.2f}  {reason[:70]}")
    ok = hit / len(states) >= 0.7
    print(f"\n   {'✅ 通过（≥70%）' if ok else '❌ 不通过（<70%）'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
