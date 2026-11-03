# 姿势画像与标定一致性

姿势画像中的特征与阈值依赖个人标定。运行时要求画像的 `baseline_id` 与当前标定一致，包含标定时间、摄像头、模型及分辨率。旧版没有绑定信息的画像也不能直接使用：界面会显示 `UNKNOWN` 和重新采集提示。用当前标定时间给旧画像补标签不能修复数据。

固定机位并使头部、双肩完整入镜，坐正后通过界面校准。采样过程中不要重新校准或移动摄像头；脚本发现标定变化会丢弃该批次。旧样本先备份，使用新目录采集：

```bash
python3 /opt/posture-guard/scripts/teach_posture.py --sample-dir /var/lib/posture-guard/posture-samples-current --label normal
python3 /opt/posture-guard/scripts/teach_posture.py --sample-dir /var/lib/posture-guard/posture-samples-current --label head_down
python3 /opt/posture-guard/scripts/teach_posture.py --sample-dir /var/lib/posture-guard/posture-samples-current --label head_tilt
python3 /opt/posture-guard/scripts/teach_posture.py --sample-dir /var/lib/posture-guard/posture-samples-current --label shoulder_tilt
python3 /opt/posture-guard/scripts/teach_posture.py --sample-dir /var/lib/posture-guard/posture-samples-current --label too_close
python3 /opt/posture-guard/scripts/teach_posture.py --sample-dir /var/lib/posture-guard/posture-samples-current --label hand_face
python3 /opt/posture-guard/scripts/teach_posture.py --sample-dir /var/lib/posture-guard/posture-samples-current --build
systemctl restart posture-guard
```

采样须由真人明确确认当前姿势。不要把自然活动自动标为正常。`--rebaseline` 不再允许将跨标定旧样本重新贴成当前标定；同一标定的特征重算仍可用 `--recompute`。

正常判定要求已测特征都处于正常样本的合理范围（各特征使用 MAD 与最小尺度中的较大者，默认容许3倍偏差）；未命中异常规则且超出正常范围时立即显示无法可靠判断，不沿用旧的正常/不良状态。缺测的手部距离不作为正常距离证据。

训练样本回放只用于排查规则。部署后必须另采独立动作验证，UNKNOWN和无效帧留在评分分母中，并检查正常误报、不良漏报、无人误检与反应时延。单帧NPU耗时不代表人体动作到播报时延。
