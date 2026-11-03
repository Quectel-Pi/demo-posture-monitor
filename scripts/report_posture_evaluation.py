#!/usr/bin/env python3
"""Recompute a human-labelled evaluation report; raw status predictions never label truth."""
import argparse
from collections import Counter
import csv
import hashlib
import importlib.util
import json
from pathlib import Path
import statistics

spec=importlib.util.spec_from_file_location('evaluation',Path(__file__).with_name('evaluate_postures.py'))
evaluation=importlib.util.module_from_spec(spec);spec.loader.exec_module(evaluation)


def pct(value):return '不可计算' if value is None else f'{100*value:.1f}%'
def sec(value):return '未命中/不可测' if value is None else f'{value:.3f}'


def report(source,output):
    data=json.loads(Path(source).read_text())
    summary=evaluation.summarize_session(data['attempts'])
    if not summary['accepted_trials']:raise ValueError('没有经过人工确认的试次，不能生成准确度结果')
    accepted=[t for t in data['attempts'] if t.get('human_confirmed')]
    output=Path(output);output.mkdir(parents=True,exist_ok=True)
    result={'metadata':data['metadata'],'complete':data.get('complete',False),'summary':summary}
    (output/'summary.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
    names={key:name for key,name,_ in evaluation.POSES}
    labels=[key for key,_,_ in evaluation.POSES]+['unknown']
    lines=['# 坐姿识别真人测试结果','',
           f"测试状态：{'协议已完成' if data.get('complete') else '部分完成'}；人工确认 {summary['accepted_trials']} 次，保存尝试 {summary['recorded_attempts']} 次。",'',
           '## 测试口径','',
           '- 真值来自操作者按预先给定动作完成后的两次确认，不使用检测结果给样本贴标签。',
           '- 每项保持 8 秒，预先排除确认后的前 1 秒；剩余所有状态帧参与统计，UNKNOWN、漏检和误判均保留。',
           '- 命中率为本次有标签状态帧上的召回率；精确率为所有预测为该类的帧中真值属于该类的比例。',
           '- 连续帧高度相关，且每类只有少量试次。这些是本机位、本人、本次动作的探索性结果，不是跨人群准确率。',
           '- “确认后稳定命中”从点击已摆好开始，要求正确状态持续至少 0.4 秒；动作可能早已被识别，此量不是物理动作起点延迟。',
           '- “阈值越线到激活”只测内部状态响应，受状态推送采样间隔影响；单次 NPU 耗时不能替代端到端延迟。','',
           '## 各姿势结果','',
           '| 姿势 | 试次 | 状态帧 | 命中率/召回率 | 精确率 | 确认后稳定命中中位数（秒） |',
           '| --- | ---: | ---: | ---: | ---: | ---: |']
    with (output/'by-pose.csv').open('w',newline='') as f:
        writer=csv.writer(f);writer.writerow(['pose','trials','status_frames','recall','precision','confirmation_response_median_s'])
        for label,row in summary['by_pose'].items():
            lines.append(f"| {names[label]} | {row['trials']} | {row['status_frames']} | {pct(row['recall'])} | {pct(row['precision'])} | {sec(row['confirmation_response_median_s'])} |")
            writer.writerow([label]+[row[k] for k in ['trials','status_frames','recall','precision','confirmation_response_median_s']])
    lines+=['','## 混淆矩阵（行：人工标签；列：系统预测；单位：状态帧）','',
            '| 人工标签 | '+' | '.join(names.get(k,'无法判定') for k in labels)+' |',
            '| --- | '+' | '.join('---:' for _ in labels)+' |']
    for label,row in summary['by_pose'].items():
        lines.append('| '+names[label]+' | '+' | '.join(str(row['confusion'].get(k,0)) for k in labels)+' |')
    lines+=['','## 逐次实时性与采样质量','',
            '| 轮次 | 姿势 | 命中率 | 目标规则激活率 | 内部阈值响应（秒） | NPU 平均 / P95（毫秒） | 状态间隔 P95（秒） | 断流/校准干扰 |',
            '| ---: | --- | ---: | ---: | ---: | ---: | ---: | --- |']
    for row in summary['trials']:
        npu='不可测' if row['npu_latency_mean_ms'] is None else f"{row['npu_latency_mean_ms']:.1f} / {row['npu_latency_p95_ms']:.1f}"
        lines.append(f"| {row['round']} | {names[row['label']]} | {pct(row['primary_hit_rate'])} | {pct(row['target_rule_hit_rate'])} | {sec(row['observed_threshold_to_rule_activation_s'])} | {npu} | {sec(row['status_interval_p95_s'])} | {'有' if row['stream_interrupted'] else '未记录到'} |")
    lines+=['','## 识别失败与混淆','']
    for label,row in summary['by_pose'].items():
        wrong={names.get(k,'无法判定'):v for k,v in row['confusion'].items() if k!=label}
        lines.append(f"- {names[label]}："+('未出现错类状态帧。' if not wrong else '、'.join(f'{k} {v} 帧' for k,v in wrong.items())+'。'))
    excluded=[t for t in data['attempts'] if not t.get('human_confirmed')]
    if excluded:
        lines+=['','## 未计入的尝试','']
        for t in excluded:lines.append(f"- 第 {t['round']} 轮 {names[t['label']]}：{t.get('excluded_reason','未人工确认')}。")
    lines+=['','## 版本与复核','']
    for key in ['config_sha256','baseline_sha256','binary_sha256']:
        if key in data['metadata']:lines.append(f"- {key}: `{data['metadata'][key]}`")
    lines+=['','原始试次见同目录 results.json；本报告由脚本从人工确认试次重新计算。']
    (output/'report.md').write_text('\n'.join(lines)+'\n')
    return summary

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('source');ap.add_argument('--out',required=True);args=ap.parse_args()
    print(json.dumps(report(args.source,args.out),ensure_ascii=False,indent=2))
