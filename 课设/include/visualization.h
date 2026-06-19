/**
 * visualization.h — 全局可视化模块
 *
 * PPT 第9页 (10分):
 *   overview — 统一视图, 严格覆盖三大核心板块:
 *     1. 进程树 (PID + 状态 + 优先级 + 内存)
 *     2. 内存 ASCII 分布图
 *     3. MLFQ 三级队列快照
 *
 *   验收标准:
 *     - 三大板块缺一不可
 *     - 执行调度/创建/销毁等操作后重新执行 overview,
 *       显示的进程状态、内存分配及队列内容必须同步变化
 */

#ifndef VISUALIZATION_H
#define VISUALIZATION_H

#include "os_types.h"

/**
 * 输出全局概览: 进程树 + 内存布局 + MLFQ 队列
 */
void overview(SystemState *s);

#endif /* VISUALIZATION_H */
