#!/usr/bin/env python3
"""从 journal 的「画像判定切换」行重建标签时间线：停留时长 / 切换矩阵 / 分段统计。

为什么不用 journal 里现成的提醒日志：提醒有 cooldown 与 hold_s 两层过滤，
不等于画像判定的真实时间线。画像切换日志才是逐帧判定的唯一出口。
"""
import json, datetime as dt, collections, statistics as st, sys

SEGS = [
    ("S1-pre-hysteresis",  "2026-11-02T09:21:36", "2026-11-02T11:16:44",
     "生产原状（profile_release_ratio 未生效 / 纯阈值）"),
    ("POLLUTION-a",        "2026-11-02T11:16:44", "2026-11-02T11:47:19",
     "我方调试：26 次重启+部署，丢弃"),
    ("S2-post-hyst-a",     "2026-11-02T11:47:19", "2026-11-02T12:59:46",
     "迟滞 0.5 生效，中途 12:48 重校准（baseline 变更）"),
    ("POLLUTION-b",        "2026-11-02T12:59:46", "2026-11-02T13:17:20",
     "我方调试：17 次重启，丢弃"),
    ("S3-post-hyst-b",     "2026-11-02T13:17:20", "2026-11-02T14:32:45",
     "迟滞 0.5 生效，干净窗口"),
]
ZH2KEY = {"正常":"normal","低头/趴桌":"head_down","歪头":"head_tilt",
          "离屏太近":"too_close","托腮":"hand_face","（无）":"NONE","无人":"away"}

def T(s): return dt.datetime.fromisoformat(s)

def load(path):
    rows=json.load(open(path))
    for r in rows:
        r["t"]=T(r["ts"]); r["ka"]=ZH2KEY.get(r["a"],r["a"]); r["kb"]=ZH2KEY.get(r["b"],r["b"])
    return rows

def episodes(rows):
    """切换行只记录「离开 A 进入 B」。停留时长 = 同一标签连续占据的区间。"""
    eps=[]
    for i,r in enumerate(rows):
        end = rows[i+1]["t"] if i+1<len(rows) else None
        eps.append(dict(label=r["kb"], start=r["t"], end=end,
                        dur=(end-r["t"]).total_seconds() if end else None,
                        entered_by=r, next_label=rows[i+1]["kb"] if i+1<len(rows) else None))
    return eps

def pct(xs,p):
    if not xs: return 0.0
    xs=sorted(xs); k=(len(xs)-1)*p; f=int(k)
    return xs[f] if f+1>=len(xs) else xs[f]+(xs[f+1]-xs[f])*(k-f)

def seg_report(rows, name, a, b, note):
    sub=[r for r in rows if T(a)<=r["t"]<T(b)]
    hrs=(T(b)-T(a)).total_seconds()/3600
    eps=episodes(sub)
    # 最后一条 episode 的 end 未知（窗口结束），其 dur 记为窗口尾 - start
    if eps: eps[-1]["dur"]=(T(b)-eps[-1]["start"]).total_seconds()
    dwell=collections.defaultdict(list)
    for e in eps:
        if e["dur"] is not None: dwell[e["label"]].append(e["dur"])
    tot=sum(sum(v) for v in dwell.values()) or 1
    print(f"\n=== {name}  {a[11:19]}→{b[11:19]}  {hrs:.2f}h  {note}")
    nn=sum(1 for r in sub if r["ka"]=="NONE")
    print(f"切换行 {len(sub)} 条 = {len(sub)/hrs:.1f}/h；其中源为（无）(会话进入) {nn} 条；"
          f"真实姿势间切换 {len(sub)-nn} = {(len(sub)-nn)/hrs:.1f}/h")
    print(f"{'标签':10s} {'episodes':>8s} {'总停留s':>9s} {'占比%':>7s} {'中位s':>7s} {'P90s':>7s} {'max_s':>7s}")
    for lab,v in sorted(dwell.items(), key=lambda kv:-sum(kv[1])):
        print(f"{lab:10s} {len(v):8d} {sum(v):9.0f} {100*sum(v)/tot:7.1f} "
              f"{st.median(v):7.1f} {pct(v,0.9):7.1f} {max(v):7.1f}")
    tm=collections.Counter((r["ka"],r["kb"]) for r in sub)
    print("切换矩阵 Top10:", ", ".join(f"{a2}->{b2}:{c}" for (a2,b2),c in tm.most_common(10)))
    # 碎跳：<3s 的 episode 占比
    short=[e for e in eps if (e["dur"] or 0)<3 and e["label"]!="NONE"]
    print(f"碎跳(<3s episode) {len(short)}/{len(eps)-sum(1 for e in eps if e['label']=='NONE')}"
          f" = {100*len(short)/max(1,len(eps)-sum(1 for e in eps if e['label']=='NONE')):.1f}%")
    return eps

if __name__=="__main__":
    rows=load(sys.argv[1])
    allout={}
    for name,a,b,note in SEGS:
        allout[name]=seg_report(rows,name,a,b,note)
    json.dump({k:[{**e,"start":e["start"].isoformat(),"end":(e["end"].isoformat() if e["end"] else None),
                   "entered_by":{kk:vv for kk,vv in e["entered_by"].items() if kk!="t"}}
                  for e in v] for k,v in allout.items()},
              open(sys.argv[2],"w"),ensure_ascii=False,indent=1)
