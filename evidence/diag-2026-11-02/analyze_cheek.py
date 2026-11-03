#!/usr/bin/env python3
"""托腮（hand_face）定量诊断：7 维 wrist_face + below_ear 门控的保留率/压制率曲线。

两类数据：
  A) 生产 journal 的 77 次托腮触发（真实使用，逐次记录 entry 帧的 5 维特征）
  B) 板端实时 status 帧 1195 帧（真实使用，含 17 关键点 → 可算 wrist_face / below_ear）
  C) /var/lib/posture-guard/posture-samples 的 114 帧人工标注（有污染，仅作对照）
"""
import json, math, statistics as st, collections
W,H=640.0,480.0
KP=["nose","eye_l","eye_r","ear_l","ear_r","sh_l","sh_r","elbow_l","elbow_r",
    "wrist_l","wrist_r","hip_l","hip_r","knee_l","knee_r","ankle_l","ankle_r"]
ZH={"正常":"normal","低头/趴桌":"head_down","歪头":"head_tilt","离屏太近":"too_close",
    "托腮":"hand_face","（无）":"NONE"}
def d(a,b): return math.hypot(a[0]-b[0],a[1]-b[1])

def P(xs,p):
    xs=sorted(xs)
    if not xs: return float("nan")
    k=(len(xs)-1)*p; i=int(k)
    return xs[i] if i+1>=len(xs) else xs[i]+(xs[i+1]-xs[i])*(k-i)

def desc(name,xs):
    med=st.median(xs); mad=st.median([abs(x-med) for x in xs])
    print(f"  {name:12s} n={len(xs):5d} min={min(xs):7.3f} P10={P(xs,.1):7.3f} "
          f"中位={med:7.3f} P90={P(xs,.9):7.3f} max={max(xs):7.3f} MAD={mad:6.3f}")
    return med,mad

print("="*100)
print("第 1 部分：生产 journal 的 77 次托腮触发（/tmp/sw3h.log，11:48→14:33，含 lead 同源数据）")
rows=json.load(open('/tmp/sw3h.json'))
for r in rows: r['kb']=ZH.get(r['b'],r['b']); r['ka']=ZH.get(r['a'],r['a'])
hf=[r for r in rows if r['kb']=='hand_face']
print(f"  托腮触发 {len(hf)} 次；进入时的 wrist_near 分布：")
desc("wrist_near",[r['f']['wrist_near'] for r in hf])
desc("wrist_rise",[r['f']['wrist_rise'] for r in hf])
desc("size",      [r['f']['size'] for r in hf])
desc("head_drop", [r['f']['head_drop'] for r in hf])

# episode 时长
dwell=[]
for i,r in enumerate(rows):
    end = rows[i+1]['ts'] if i+1<len(rows) else None
    dwell.append((r['kb'], r['ts'], end))
import datetime as dt
def T(s): return dt.datetime.fromisoformat(s)
eps=[]
for i,(lab,a,b) in enumerate(dwell):
    e = T(b) if b else T(rows[-1]['ts'])
    eps.append(dict(lab=lab,start=a,dur=(e-T(a)).total_seconds(), entry=(rows[i]['f'] if lab!='NONE' else {})))
hfe=[e for e in eps if e['lab']=='hand_face']
print(f"\n  托腮 episode 数 {len(hfe)}；时长分位：P50={P([e['dur'] for e in hfe],.5):.1f}s "
      f"P90={P([e['dur'] for e in hfe],.9):.1f}s max={max(e['dur'] for e in hfe):.0f}s")
LONG=[e for e in hfe if e['dur']>=30]
SHORT=[e for e in hfe if e['dur']<5]
print(f"  长命(>=30s) {len(LONG)} 个，短命(<5s) {len(SHORT)} 个")
print(f"  长命 entry wrist_near 中位 {st.median([e['entry']['wrist_near'] for e in LONG]):.3f}")
print(f"  短命 entry wrist_near 中位 {st.median([e['entry']['wrist_near'] for e in SHORT]):.3f}")
print(f"  长命 entry wrist_rise 中位 {st.median([e['entry']['wrist_rise'] for e in LONG]):.3f}")
print(f"  短命 entry wrist_rise 中位 {st.median([e['entry']['wrist_rise'] for e in SHORT]):.3f}")
print("\n  超长 episode（判「真托腮」还是「黏住」）：")
for e in sorted(hfe,key=lambda x:-x['dur'])[:6]:
    print(f"    dur={e['dur']:7.1f}s start={e['start'][11:19]} entry: "
          f"wrist_near={e['entry']['wrist_near']:.3f} wrist_rise={e['entry']['wrist_rise']:.3f} "
          f"size={e['entry']['size']:.3f} head_drop={e['entry']['head_drop']:.3f} "
          f"ear_tilt={e['entry']['ear_tilt']:.1f}")

print("\n" + "="*100)
print("第 2 部分：门控曲线 —— 用 wrist_near 门控生产 77 次托腮触发")
print("  「压制率」= 被门控挡掉的触发占比；「保留率」= 长命(>=30s，疑似真托腮) 留下的比例")
for thr in (0.4,0.5,0.6,0.7,0.8,0.9,1.0,1.1,1.2):
    sup = sum(1 for r in hf if r['f']['wrist_near']>thr)
    keep_long = sum(1 for e in LONG if e['entry']['wrist_near']<=thr)
    keep_short= sum(1 for e in SHORT if e['entry']['wrist_near']<=thr)
    print(f"  wrist_near <= {thr:.1f} : 压制 {sup:2d}/{len(hf)} = {100*sup/len(hf):5.1f}%   "
          f"长命保留 {keep_long}/{len(LONG)} = {100*keep_long/max(1,len(LONG)):5.1f}%   "
          f"短命保留 {keep_short}/{len(SHORT)} = {100*keep_short/max(1,len(SHORT)):5.1f}%")

print("\n" + "="*100)
print("第 3 部分：板端实时帧 1195 帧（真实使用，含 wrist_face / below_ear）")
live=[json.loads(l) for l in open('evidence/diag-2026-11-02/raw/live-replay.json')]
newf=[]
for r in live:
    if not r.get('kp'): continue
    f=r['f']
    sh_l=(r['kp'][10]*W,r['kp'][11]*H); sh_r=(r['kp'][12]*W,r['kp'][13]*H)
    nose=(r['kp'][0]*W,r['kp'][1]*H)
    ear_l=(r['kp'][6]*W,r['kp'][7]*H); ear_r=(r['kp'][8]*W,r['kp'][9]*H)
    wl=(r['kp'][18]*W,r['kp'][19]*H); wr=(r['kp'][20]*W,r['kp'][21]*H)
    sw=d(sh_l,sh_r)
    em=((ear_l[0]+ear_r[0])/2,(ear_l[1]+ear_r[1])/2)
    w = wl if d(wl,nose)<=d(wr,nose) else wr
    f['wrist_face']=d(w,em)/sw
    f['below_ear']=(w[1]-em[1])/sw
    newf.append(f)
print(f"  有效帧 {len(newf)}")
desc("wrist_near",[f['wrist_near'] for f in newf])
desc("wrist_face",[f['wrist_face'] for f in newf])
desc("below_ear", [f['below_ear'] for f in newf])
hf_live=[f for f in newf if f['wrist_rise']>=0.286]
print(f"\n  按生产 rule4（wrist_rise>=0.286）判为托腮的帧：{len(hf_live)}/{len(newf)} = {100*len(hf_live)/len(newf):.1f}%")
if hf_live:
    desc("→wrist_face",[f['wrist_face'] for f in hf_live])
    desc("→wrist_near",[f['wrist_near'] for f in hf_live])
    desc("→below_ear",[f['below_ear'] for f in hf_live])
print("\n  加 wrist_face 门控（生产未上线）后剩余：")
for thr in (0.5,0.6,0.7,0.8,0.9,1.0):
    k=sum(1 for f in hf_live if f['wrist_face']<=thr)
    print(f"    wrist_face <= {thr:.1f} : 保留 {k:4d}/{len(hf_live)} = {100*k/max(1,len(hf_live)):5.1f}% "
          f"（占全部帧 {100*k/len(newf):5.1f}%）")

print("\n" + "="*100)
print("第 4 部分：人工标注 114 帧（板端 posture-samples，含 wrist_to_nose_raw）")
import subprocess
samp={}
for lab in ("normal","head_down","head_tilt","hand_face","too_close"):
    out=subprocess.run(["adb","-t","68","shell",f"cat /var/lib/posture-guard/posture-samples/{lab}.json"],
                       capture_output=True,text=True).stdout
    samp[lab]=json.loads(out)
for lab,d in samp.items():
    near=[min(r['wrist_to_nose_raw'],3.0) for r in d['raw_metrics']]
    rise=[r['wrist_y_rel'] for r in d['raw_metrics']]
    med,mad=desc(f"{lab}.near",near)
print("\n  ★ 关键对照：真标注手托腮 vs 生产触发")
print(f"    标注 hand_face 的 wrist_near 中位 {st.median([min(r['wrist_to_nose_raw'],3.0) for r in samp['hand_face']['raw_metrics']]):.3f}")
print(f"    生产 77 次触发的 wrist_near 中位 {st.median([r['f']['wrist_near'] for r in hf]):.3f}")
print(f"    生产实时帧的 wrist_near 中位 {st.median([f['wrist_near'] for f in newf]):.3f}")
