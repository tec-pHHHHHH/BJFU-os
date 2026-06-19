/**
 * scheduler.h — 多级反馈队列调度器
 *
 * 4 条命令 (8分, PPT 第5页):
 *   start_sched   — 启动自动调度 (2分)
 *   stop_sched    — 暂停调度, 保留现场 (2分)
 *   restart_sched — 恢复调度 (2分)
 *   step          — 单步执行, 展示完整决策链路 (2分)
 *
 * 红线: 必须真实动态执行 — 实时日志 + 时间推进 + 队列降级
 */

#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "os_types.h"

/* 调度器线程 */
void *scheduler_thread_func(void *arg);

/* 命令接口 */
int  scheduler_start(SystemState *s);
int  scheduler_stop(SystemState *s);
int  scheduler_restart(SystemState *s);
int  scheduler_step(SystemState *s);

/* 内部: 执行一轮调度 (选进程 → 运行 → 日志 → 降级/重新入队) */
void scheduler_tick(SystemState *s);

/* 辅助: 打印当前 MLFQ 快照 */
void scheduler_show_queues(SystemState *s);

#endif /* SCHEDULER_H */
