#!/usr/bin/env python3
"""托腮第 7 维 wrist_face 为什么几乎无分离度：手腕「二选一」选错了。

生产代码 ComputeMetrics 里，先按「离鼻子最近」选一只手腕（best），
再把这个 best 手腕到耳中点的距离当 wrist_face。但板端 joint_confidence
不可信（RknnPoseEngine::joint_confidence_reliable() 恒 false，w8a8 把 conf 通道
量化成 0），所以这个「二选一」没有任何置信度保护 —— 搭在扶手上的那只手
只要离鼻子稍近就会被选中，wrist_face 立刻被拉到 1.1 以上。
"""
import json, math, statistics as st
W,H=640.0,480.0
def d(a,b): return math.hypot(a[0]-b[0],a[1]-b[1])
def P(xs,p):
    xs=sorted(xs)
    if not xs: return float('nan')
    k=(len(xs)-1)*p; i=int(k)
    return xs[i] if i+1>=len(xs) else xs[i]+(xs[i+1]-xs[i])*(k-i)
def desc(n,xs):
    med=st.median(xs); print(f"  {n:22s} n={len(xs):5d} min={min(xs):6.3f} P10={P(xs,.1):6.3f} "
          f"中位={med:6.3f} P90={P(xs,.9):6.3f} max={max(xs):6.3f} MAD={st.median([abs(x-med) for x in xs]):6.3f}")
    return med

live=[json.loads(l) for l in open('evidence/diag-2026-11-02/raw/live-replay.json')]
recs=[]
for r in live:
    if not r.get('kp'): continue
    k=r['kp']
    nose=(k[0]*W,k[1]*H); ear_l=(k[6]*W,k[7]*H); ear_r=(k[8]*W,k[9]*H)
    sh_l=(k[10]*W,k[11]*H); sh_r=(k[12]*W,k[13]*H)
    wl=(k[18]*W,k[19]*H); wr=(k[20]*W,k[21]*H)
    sw=d(sh_l,sh_r); em=((ear_l[0]+ear_r[0])/2,(ear_l[1]+ear_r[1])/2)
    # 生产口径：按离鼻子最近选一只手
    sel = wl if d(wl,nose)<=d(wr,nose) else wr
    recs.append(dict(
        t=r['t'], state=r['posture_state'],
        rule_wrist_face=d(sel,em)/sw,
        rule_below_ear=(sel[1]-em[1])/sw,
        min_wrist_face=min(d(wl,em),d(wr,em))/sw,        # 两只手里更贴脸的那只
        max_below_ear=max((wl[1]-em[1])/sw,(wr[1]-em[1])/sw),
        wl_face=d(wl,em)/sw, wr_face=d(wr,em)/sw,
        wl_near=d(wl,nose)/sw, wr_near=d(wr,nose)/sw,
        rise=r['f']['wrist_rise'], near=r['f']['wrist_near'],
    ))
print(f"实时帧 {len(recs)}")
hf=[x for x in recs if x['rise']>=0.286]
nf=[x for x in recs if x['rise']<0.286]
print(f"生产 rule4 命中（wrist_rise>=0.286）{len(hf)} 帧；未命中 {len(nf)} 帧\n")
print("【A】生产口径 wrist_face = 离鼻子最近那只腕 → 耳中点")
desc("全部帧",[x['rule_wrist_face'] for x in recs])
desc("命中帧",[x['rule_wrist_face'] for x in hf])
print("【B】两只手里更贴脸的那只（min）")
desc("全部帧",[x['min_wrist_face'] for x in recs])
desc("命中帧",[x['min_wrist_face'] for x in hf])
print("【C】相邻左右手 wrist_face 差（说明二选一很关键）")
desc("|wl-wr| 差",[abs(x['wl_face']-x['wr_face']) for x in recs])

print("\n门控曲线（对实时帧，命中 715 帧中能留下多少；同时看非命中帧被误留多少）")
print(f"{'门控':28s} {'命中保留':>9s} {'非命中误留':>10s} {'分离比':>8s}")
for name,key,thr in [("A口径 wrist_face<=0.95","rule_wrist_face",0.95),
                     ("A口径 wrist_face<=0.80","rule_wrist_face",0.80),
                     ("A口径 wrist_face<=0.70","rule_wrist_face",0.70),
                     ("B口径 min wrist_face<=0.95","min_wrist_face",0.95),
                     ("B口径 min wrist_face<=0.80","min_wrist_face",0.80),
                     ("B口径 min wrist_face<=0.70","min_wrist_face",0.70),
                     ("B口径 min wrist_face<=0.60","min_wrist_face",0.60)]:
    kh=sum(1 for x in hf if x[key]<=thr); kn=sum(1 for x in nf if x[key]<=thr)
    print(f"{name:28s} {kh:4d}/{len(hf)}={100*kh/len(hf):5.1f}% {kn:4d}/{len(nf)}={100*kn/len(nf):5.1f}% "
          f"{'':>8s}")
