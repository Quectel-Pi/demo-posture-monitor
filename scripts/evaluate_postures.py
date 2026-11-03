#!/usr/bin/env python3
"""Human-labelled live posture evaluation. Never infers ground truth from predictions.

Displays camera frames in memory only; saves status metrics, human confirmations,
notification events and protocol timestamps. Does not change calibration/config.
"""
from __future__ import annotations
import argparse
from collections import Counter
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import statistics
import time

POSES = [
    ('normal', '正常坐姿', '坐直、头摆正、双肩放平，双手自然放在桌面，目视屏幕。'),
    ('head_down', '低头', '保持双肩不动，像看键盘或手机一样低头。不要同时歪头或托腮。'),
    ('head_tilt', '歪头', '双肩放平，头向{side}侧倾斜约 20–30 度，耳朵朝肩膀。不要转脸或低头。'),
    ('shoulder_tilt', '高低肩', '头尽量摆正，将{side}侧肩膀明显抬高，形成高低肩。不要同时前倾。'),
    ('hand_on_face', '托腮', '用{side}手托住下巴或脸颊，肘部支撑桌面。尽量维持头肩位置。'),
    ('too_close', '凑近屏幕', '身体向屏幕前倾，保持头肩完整入画。不要同时低头或用手托脸。'),
    ('away', '离开画面', '点击后有 5 秒离座准备时间。随后离开摄像头画面，保持离开直到采样结束，再回来确认。'),
]
STATE_LABELS = {'NORMAL': 'normal', 'HEAD_DOWN': 'head_down', 'HEAD_TILT': 'head_tilt',
                'SHOULDER_TILT': 'shoulder_tilt', 'HAND_ON_FACE': 'hand_on_face',
                'TOO_CLOSE': 'too_close', 'USER_AWAY': 'away', 'UNKNOWN': 'unknown'}
FIELDS = ('posture_state', 'metrics_valid', 'present', 'persons', 'persons_primary', 'fps', 'latency_ms', 'frame_id',
          'has_baseline', 'calibrating', 'baseline_created_at', 'profile_active',
          'active_rules', 'rules', 'explanation', 'frame_width', 'frame_height',
          'session_state', 'use_s', 'remaining_s', 'snoozed')


def percentile(values, fraction):
    if not values:
        return None
    items = sorted(values)
    return items[min(len(items)-1, math.ceil(fraction * len(items))-1)]


def first_stable_match(rows, expected, origin, hold=.4):
    since = None
    previous = None
    for row in rows:
        t = row['t']
        if t < origin:
            continue
        if STATE_LABELS.get(row['data'].get('posture_state'), 'unknown') != expected:
            since = previous = None
            continue
        if since is None or (previous is not None and t - previous > 1.0):
            since = t
        previous = t
        if t - since >= hold:
            return round(t - origin, 3)
    return None


def threshold_response(trial):
    """Status-observed threshold crossing to target rule latch; not physical onset."""
    key = trial['label']
    if key in ('normal','away'):
        return None
    rows = trial.get('baseline_reference',[]) + trial.get('transition',[]) + trial['samples']
    previous_bad = None
    candidate_at = None
    previous_t = None
    for row in rows:
        r = next((r for r in row['data'].get('rules',[]) if r.get('key') == key), None)
        if not r or not r.get('has_value') or not row['data'].get('metrics_valid'):
            previous_bad = candidate_at = previous_t = None
            continue
        if previous_t is not None and row['t'] - previous_t > 1:
            previous_bad = candidate_at = None
        previous_t = row['t']
        bad = r['value'] < r['trigger'] if key == 'hand_on_face' else r['value'] > r['trigger']
        if row['t'] >= trial['cue_at'] and bad and previous_bad is False and not r.get('latched'):
            candidate_at = row['t']
        if not bad:
            candidate_at = None
        if candidate_at is not None and r.get('latched'):
            return round(row['t'] - candidate_at, 3)
        previous_bad = bad
    return None


def summarize_trial(trial):
    expected = trial['label']
    # A declared 1s settling interval; invalid/unknown frames stay in denominators.
    rows = [r for r in trial['samples'] if r['t'] >= trial['confirmed_at'] + 1.0]
    counts = Counter(STATE_LABELS.get(r['data'].get('posture_state'), 'unknown') for r in rows)
    n = len(rows)
    active_hit = sum(expected in (r['data'].get('active_rules') or []) for r in rows)
    if expected in ('normal', 'away'):
        active_hit = counts[expected]
    transitions = trial.get('transition', []) + trial['samples']
    latencies = [r['data']['latency_ms'] for r in rows if isinstance(r['data'].get('latency_ms'), (int,float))]
    gaps = [b['t']-a['t'] for a,b in zip(rows,rows[1:])]
    return {
        'label': expected, 'round': trial['round'], 'human_confirmed': trial.get('human_confirmed', False),
        'stream_interrupted': trial.get('stream_interrupted', False),
        'scored_status_frames': n, 'correct': counts[expected], 'state_counts': dict(counts),
        'primary_hit_rate': counts[expected]/n if n else None,
        'target_rule_hit_rate': active_hit/n if n else None,
        'valid_geometry_rate': sum(bool(r['data'].get('metrics_valid')) for r in rows)/n if n else None,
        'cue_to_stable_match_s_includes_human_motion': first_stable_match(transitions, expected, trial['cue_at']),
        'confirmation_to_stable_match_s': first_stable_match(trial['samples'], expected, trial['confirmed_at']),
        'observed_threshold_to_rule_activation_s': threshold_response(trial),
        'npu_latency_mean_ms': statistics.mean(latencies) if latencies else None,
        'npu_latency_p95_ms': percentile(latencies,.95),
        'status_interval_p95_s': percentile(gaps,.95),
    }


def summarize_session(trials):
    accepted = [t for t in trials if t.get('human_confirmed')]
    matrix = {}
    for trial in accepted:
        summ = summarize_trial(trial)
        counts = matrix.setdefault(trial['label'], Counter())
        counts.update(summ['state_counts'])
    output = {}
    for label, _, _ in POSES:
        relevant = [summarize_trial(t) for t in accepted if t['label']==label]
        if not relevant:
            continue
        total = sum(r['scored_status_frames'] for r in relevant)
        tp = matrix[label][label]
        predicted = sum(row[label] for row in matrix.values())
        response = [r['confirmation_to_stable_match_s'] for r in relevant if r['confirmation_to_stable_match_s'] is not None]
        output[label] = {'trials':len(relevant), 'status_frames':total, 'correct':tp,
                         'recall':tp/total if total else None,
                         'precision':tp/predicted if predicted else None,
                         'stable_matches':len(response),
                         'confirmation_response_median_s':statistics.median(response) if response else None,
                         'confusion':dict(matrix[label])}
    return {'accepted_trials':len(accepted), 'recorded_attempts':len(trials),
            'by_pose':output, 'trials':[summarize_trial(t) for t in accepted]}


def run_gui(args):
    import gi
    gi.require_version('Gtk','4.0')
    gi.require_version('Gdk','4.0')
    from gi.repository import Gdk, Gio, GLib, Gtk
    spec = importlib.util.spec_from_file_location('posture_ui',Path(__file__).resolve().parents[1]/'ui/posture_guard_ui.py')
    ui = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ui)
    output = Path(args.out)
    output.mkdir(parents=True, exist_ok=True)
    metadata = {'protocol':'human-confirmed-v1','rounds':args.rounds,'capture_s':args.seconds,
                'settle_excluded_s':1.0,'configuration_modified':False,'raw_video_saved':False,
                'latency_note':'cue latency includes human motion; confirmation latency starts after subject says pose is ready; neither is independently video-annotated onset latency'}
    for name,path in [('config','/etc/posture-guard/posture.yaml'),('baseline','/var/lib/posture-guard/baseline.json'),('binary','/opt/posture-guard/bin/posture_guard')]:
        try:
            content=Path(path).read_bytes()
            metadata[name+'_sha256']=hashlib.sha256(content).hexdigest()
            if name=='baseline': metadata['baseline']=json.loads(content)
        except OSError: pass
    records=[]
    plan=[(r,key,name,hint.format(side='左' if r%2 else '右')) for r in range(1,args.rounds+1) for key,name,hint in POSES]

    class App(Gtk.Application):
        def __init__(self):
            super().__init__(application_id='com.quectel.PostureEvaluation',flags=Gio.ApplicationFlags.NON_UNIQUE)
        def do_activate(self):
            self.win=Window(self)
            self.win.present()
            self.win.fullscreen()

    class Window(Gtk.ApplicationWindow):
        def __init__(self,app):
            super().__init__(application=app,title='坐姿准确度与实时性测试')
            self.index=0
            self.phase='prepare_baseline'
            self.phase_at=time.monotonic()
            self.current=None
            self.transition=[]
            self.baseline_rows=[]
            self.last_status=None
            self.last_status_at=0
            self.last_video_at=0
            self.pending_video=None
            self.closed=False
            self.connected_once=False
            self.last_error=''
            self.changed_during_test=False
            self.video_idle=False
            self.buttons=[]
            provider=Gtk.CssProvider()
            provider.load_from_data(b'''
window {background:#0c1422;color:#e5edf8;}
.header {padding:14px 24px;background:#14243a;}
.brand {font-size:22px;font-weight:800;}
.title {font-size:29px;font-weight:800;color:#7fe0ca;}
.hint {font-size:20px;color:#e2ebf7;}
.small {font-size:14px;color:#a4b7cf;}
.panel {background:#14243a;padding:22px;border-radius:14px;}
button {background:#24466b;background-image:none;color:#ffffff;border:1px solid #5b7da2;border-radius:9px;padding:13px 20px;font-size:18px;}
button label {color:#ffffff;}
button:disabled {opacity:.4;}
progressbar trough {min-height:12px;background:#253d59;}
progressbar progress {min-height:12px;background:#63cbb4;}
''')
            Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(),provider,Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
            root=Gtk.Box(orientation=Gtk.Orientation.VERTICAL,spacing=12)
            self.set_child(root)
            self.heading=Gtk.Label(label='坐姿准确度与实时性测试',xalign=0)
            self.heading.add_css_class('brand');self.heading.add_css_class('header')
            root.append(self.heading)
            body=Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL,spacing=18)
            body.set_vexpand(True)
            for side in ('start','end','bottom'):
                getattr(body,'set_margin_'+side)(18)
            root.append(body)
            self.picture=Gtk.Picture()
            self.picture.set_content_fit(Gtk.ContentFit.CONTAIN)
            self.picture.set_hexpand(True);self.picture.set_vexpand(True)
            body.append(self.picture)
            panel=Gtk.Box(orientation=Gtk.Orientation.VERTICAL,spacing=18)
            panel.add_css_class('panel');panel.set_size_request(420,-1)
            body.append(panel)
            self.stage=Gtk.Label(xalign=0,wrap=True);self.stage.add_css_class('title');panel.append(self.stage)
            self.hint=Gtk.Label(xalign=0,wrap=True);self.hint.add_css_class('hint');panel.append(self.hint)
            self.note=Gtk.Label(xalign=0,wrap=True);self.note.add_css_class('small');panel.append(self.note)
            self.progress=Gtk.ProgressBar();panel.append(self.progress)
            self.status=Gtk.Label(xalign=0,wrap=True);self.status.add_css_class('small');panel.append(self.status)
            self.primary=Gtk.Button(label='开始');self.primary.connect('clicked',self.clicked);panel.append(self.primary)
            self.retry=Gtk.Button(label='刚才没做好，重测');self.retry.connect('clicked',self.retest);panel.append(self.retry)
            self.skip=Gtk.Button(label='跳过本项');self.skip.connect('clicked',self.skip_trial);panel.append(self.skip)
            self.stop_btn=Gtk.Button(label='暂停并保存退出');self.stop_btn.connect('clicked',lambda *_: self.close());panel.append(self.stop_btn)
            privacy=Gtk.Label(label='只保存状态和人工确认，不保存摄像头画面。\n为避免影响动作，测试中不播报姿势纠正语音。',xalign=0,wrap=True)
            privacy.add_css_class('small');panel.append(privacy)
            self.client=ui.SocketClient(args.socket,self.on_status,self.on_event,self.on_video)
            self.client.start()
            self.connect('close-request',self.on_close)
            self.prepare()
            GLib.timeout_add(100,self.tick)

        def save(self,complete=False):
            payload={'metadata':metadata,'complete':complete,'summary':summarize_session(records),'attempts':records,
                     'current_trial':self.current if self.phase in ('capture','confirm') else None}
            temp=output/'results.json.tmp';temp.write_text(json.dumps(payload,ensure_ascii=False,indent=2));temp.replace(output/'results.json')
            phase={'index':self.index,'total':len(plan),'phase':self.phase,'label':plan[self.index][1] if self.index<len(plan) else None,
                   'accepted':sum(bool(t.get('human_confirmed')) for t in records),'complete':complete,
                   'last_status_age_s':round(time.monotonic()-self.last_status_at,2) if self.last_status_at else None,
                   'config_or_baseline_changed':self.changed_during_test}
            temp=output/'progress.json.tmp';temp.write_text(json.dumps(phase,ensure_ascii=False));temp.replace(output/'progress.json')

        def prepare(self):
            if self.index>=len(plan):
                self.phase='complete'
                self.stage.set_text('两轮测试已完成')
                self.hint.set_text('结果已保存。点击“完成并返回检测界面”，我会整理各姿势的命中率、误判及响应时间。')
                self.primary.set_label('完成并返回检测界面');self.primary.set_sensitive(True)
                self.retry.set_visible(False);self.skip.set_visible(False)
                self.save(True);return
            round_,key,name,hint=plan[self.index]
            self.current=None;self.transition=[];self.baseline_rows=[]
            self.phase='prepare_target' if key=='normal' else 'prepare_baseline'
            self.phase_at=time.monotonic()
            self.heading.set_text(f'第 {round_} 轮 / {args.rounds} 轮  ·  第 {self.index+1} / {len(plan)} 项  ·  {name}')
            self.stage.set_text('先恢复正常坐姿' if key!='normal' else '正常坐姿')
            self.hint.set_text('坐直，头摆正，双肩放平，双手离开脸部。准备好后点击下方按钮。' if key!='normal' else hint)
            self.note.set_text('结果在采样结束后统计；请按动作要求做，不要迎合识别结果。')
            self.primary.set_label('已坐正，准备本项' if key!='normal' else '已摆好，开始采样')
            self.primary.set_sensitive(True);self.retry.set_visible(False);self.skip.set_visible(True)
            self.progress.set_fraction(0)
            self.save()

        def target_prompt(self):
            _,key,name,hint=plan[self.index]
            self.phase='prepare_target';self.phase_at=time.monotonic();self.cue_at=self.phase_at
            self.stage.set_text('现在做：'+name);self.hint.set_text(hint)
            self.primary.set_label('开始离座倒计时（5 秒）' if key=='away' else '已摆好，开始采样')
            self.primary.set_sensitive(True);self.progress.set_fraction(0);self.save()

        def clicked(self,*_):
            now=time.monotonic()
            if self.phase=='complete':self.close();return
            if self.phase=='confirm':
                self.current['human_confirmed']=True
                records.append(self.current);self.index+=1;self.prepare();return
            if not self.client.connected or now-self.last_status_at>2:
                self.note.set_text('尚未收到实时状态，请等待连接恢复后重试。');return
            if self.changed_during_test:
                self.note.set_text('检测基准在测试中发生变化。本轮请停止，避免混合不同基准的结果。');return
            if self.phase=='prepare_baseline':
                self.phase='baseline_capture';self.phase_at=now
                self.stage.set_text('保持正常坐姿，准备 3 秒')
                self.primary.set_sensitive(False);self.save();return
            if self.phase=='prepare_target':
                if plan[self.index][1]=='away':
                    self.phase='away_countdown';self.phase_at=now;self.cue_at=now
                    self.primary.set_sensitive(False);self.save();return
                if plan[self.index][1]=='normal':self.cue_at=now
                self.start_capture(now)

        def start_capture(self,now):
            r,key,name,hint=plan[self.index]
            self.current={'round':r,'label':key,'cue_at':self.cue_at,'confirmed_at':now,
                          'confirmation_method':'post-capture human confirmation after 5s leave cue' if key=='away' else 'subject clicked pose ready; confirmed again after capture',
                          'samples':[],'transition':list(self.transition),'baseline_reference':list(self.baseline_rows),
                          'events':[],'human_confirmed':False,'stream_interrupted':False}
            self.phase='capture';self.phase_at=now
            self.stage.set_text('保持：'+name)
            self.hint.set_text(f'请保持当前动作 {args.seconds:g} 秒，等待采样结束。')
            self.primary.set_sensitive(False);self.skip.set_sensitive(False)
            self.save()

        def retest(self,*_):
            if self.current:
                self.current['human_confirmed']=False;self.current['excluded_reason']='subject rejected execution'
                records.append(self.current)
            self.skip.set_sensitive(True);self.prepare()

        def skip_trial(self,*_):
            r,key,_,_=plan[self.index]
            records.append({'round':r,'label':key,'human_confirmed':False,'excluded_reason':'subject skipped',
                            'samples':[],'transition':[],'confirmed_at':time.monotonic(),'cue_at':time.monotonic()})
            self.index+=1;self.prepare()

        def on_status(self,msg):GLib.idle_add(self.apply_status,msg,time.monotonic())
        def apply_status(self,msg,received_at):
            self.last_status=msg;self.last_status_at=received_at
            if metadata.get('baseline') and msg.get('baseline_created_at')!=metadata['baseline'].get('created_at'):
                self.changed_during_test=True
            row={'t':received_at,'data':{k:msg.get(k) for k in FIELDS}}
            if self.phase=='baseline_capture':self.baseline_rows.append(row)
            elif self.phase in ('prepare_target','away_countdown'):
                self.transition.append(row)
            elif self.phase=='capture':
                self.current['samples'].append(row)
                if msg.get('calibrating'):self.current['stream_interrupted']=True
            return False

        def on_event(self,msg):GLib.idle_add(self.apply_event,msg,time.monotonic())
        def apply_event(self,msg,received_at):
            if self.phase=='capture':self.current['events'].append({'t':received_at,'data':msg})
            return False

        def on_video(self,msg):
            self.pending_video=msg
            if not self.video_idle:
                self.video_idle=True;GLib.idle_add(self.apply_video)
        def apply_video(self):
            import base64
            self.video_idle=False;msg=self.pending_video;self.pending_video=None
            if msg:
                try:
                    texture=Gdk.Texture.new_from_bytes(GLib.Bytes.new(base64.b64decode(msg['jpeg'])))
                    self.picture.set_paintable(texture);self.last_video_at=time.monotonic()
                except Exception as exc:self.last_error=str(exc)
            return False

        def tick(self):
            if self.closed:return False
            now=time.monotonic()
            age=now-self.last_status_at if self.last_status_at else 999
            self.status.set_text(f'状态连接：{"正常" if self.client.connected and age<2 else "中断"}  ·  已人工确认 {sum(bool(t.get("human_confirmed")) for t in records)} 项')
            if self.phase in ('baseline_capture','away_countdown','capture'):
                duration=3 if self.phase=='baseline_capture' else 5 if self.phase=='away_countdown' else args.seconds
                elapsed=now-self.phase_at;left=max(0,math.ceil(duration-elapsed))
                self.progress.set_fraction(min(1,elapsed/duration))
                self.note.set_text(f'剩余 {left} 秒 · 请保持动作，采样不会因检测成功而提前结束。')
                if self.phase=='capture' and (age>2 or not self.client.connected):self.current['stream_interrupted']=True
                if elapsed>=duration:
                    if self.phase=='baseline_capture':self.target_prompt()
                    elif self.phase=='away_countdown':self.start_capture(now)
                    else:
                        self.phase='confirm'
                        self.current['ended_at']=now
                        self.stage.set_text('采样结束，请确认动作')
                        self.hint.set_text('刚才是否全程按要求保持了该姿势？如果没有做到，请重测；不要根据系统是否识别正确来选择。')
                        self.primary.set_label('动作符合要求，记录本项')
                        self.primary.set_sensitive(True);self.retry.set_visible(True);self.skip.set_sensitive(True)
                        self.save()
            # Persist phase changes and interrupted state regularly for remote progress inspection.
            if int(now*10)%10==0:self.save(self.phase=='complete')
            return True

        def on_close(self,*_):
            self.closed=True
            if self.current and self.phase=='capture':
                self.current['excluded_reason']='test closed during capture';records.append(self.current)
            self.save(self.phase=='complete');self.client.stop();return False

    app=App()
    app.run([])
    return 0


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--socket',default='/run/posture-guard/status.sock')
    ap.add_argument('--out',required=True)
    ap.add_argument('--rounds',type=int,default=2)
    ap.add_argument('--seconds',type=float,default=8)
    ap.add_argument('--summarize',action='store_true')
    args=ap.parse_args()
    if args.summarize:
        payload=json.loads((Path(args.out)/'results.json').read_text())
        print(json.dumps(summarize_session(payload['attempts']),ensure_ascii=False,indent=2));return 0
    if args.rounds<1 or args.seconds<3:ap.error('rounds >= 1 and seconds >= 3 required')
    return run_gui(args)

if __name__=='__main__':raise SystemExit(main())
