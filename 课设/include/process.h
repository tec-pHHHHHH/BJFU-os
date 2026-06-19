/**
 * process.h — 进程管理模块
 *
 * 10 条进程命令 (20分):
 *   create_pcb, kill_pcb, block_pcb, wakeup_pcb, show_pcb,
 *   list_pcb, ptree, suspend, resume, renice
 */

#ifndef PROCESS_H
#define PROCESS_H

#include "os_types.h"

/* 错误码 */
#define PROC_OK            0   /* 成功 */
#define PROC_NOT_FOUND    -1   /* 进程不存在 */
#define PROC_INVALID_PID  -2   /* PID 非法 */
#define PROC_NO_PERM      -3   /* 无权操作(不属于当前用户) */
#define PROC_FULL         -4   /* 进程数已满 */
#define PROC_BAD_STATE    -5   /* 状态不允许该操作 */
#define PROC_NO_MEM       -6   /* 无法分配内存(init进程) */

/* 状态辅助 */
const char* pcb_state_string(ProcessState state);

/* 查找 */
PCB* pcb_find(SystemState *s, int pid);

/* 进程操作 */
int  pcb_create(SystemState *s, const char *name, int priority);
int  pcb_kill(SystemState *s, int pid);
int  pcb_block(SystemState *s, int pid);
int  pcb_wakeup(SystemState *s, int pid);
int  pcb_suspend(SystemState *s, int pid);
int  pcb_resume(SystemState *s, int pid);
int  pcb_renice(SystemState *s, int pid, int new_priority);

/* 显示 */
void pcb_show(SystemState *s, int pid);
void pcb_list(SystemState *s);
void pcb_ptree(SystemState *s);

/* MLFQ 内部操作 */
void pcb_enqueue_ready(SystemState *s, PCB *p);
void pcb_dequeue(SystemState *s, PCB *p);

#endif /* PROCESS_H */
