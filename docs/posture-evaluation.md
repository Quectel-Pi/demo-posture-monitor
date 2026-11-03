# 真人准确度与实时性测试

在板端桌面会话启动 `scripts/evaluate_postures.py`。它显示实时取景和动作要求，
只记录状态数据、通知事件和人工确认，不保存摄像头图片或视频，也不修改检测配置与基准。

```bash
python3 scripts/evaluate_postures.py --out /tmp/posture-evaluation --rounds 2 --seconds 8
python3 scripts/report_posture_evaluation.py /tmp/posture-evaluation/results.json --out /tmp/posture-evaluation/report
python3 tests/test_evaluation.py
```

每轮：正常、低头、歪头、高低肩、托腮、凑近、离开。第二轮歪头、高低肩和托腮换边。
每项先恢复正常，再按要求摆姿势；操作者点击已摆好后采样 8 秒，结束后确认动作是否符合要求。
离开测试提供 5 秒离座准备时间，返回后再确认。未完成动作可以重测或跳过，尝试会保留，
不因为模型是否识别成功而自动决定记录或丢弃。

准确度以人工动作标签为真值，不用模型输出贴标签；预先排除确认后前 1 秒，
剩余状态帧中的 UNKNOWN、漏检和错类都留在分母内。汇总每类主状态召回率、精确率、
目标规则激活率和混淆矩阵。连续状态帧不是独立样本，两轮测试只能描述本机位本次表现。
单人测试应避免其他人进入画面；发生多人干扰、断流或校准变化时要单独说明。

实时性分为：NPU 单次推理耗时、状态更新间隔、人工确认后的稳定命中时间、
内部规则越线到状态激活时间。提示到命中的时间包含人的反应与动作执行；
确认后的时间可能从识别已完成之后才开始。两者都不能替代独立视频标注的物理动作起点延迟。
没有观察到阈值越线，或没有命中时记录为不可测，不填 0。

结果保存在输出目录的 results.json、progress.json。只有操作者确认的试次才用于汇总，
没有人工确认试次时，报告脚本拒绝生成准确度数字。

本次设备测试目录：`/var/lib/posture-guard/evaluations/live-20261029-71fgssge/`。
退出测试向导后，测试启动器恢复原来的检测界面。
