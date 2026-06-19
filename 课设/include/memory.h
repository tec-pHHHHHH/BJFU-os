/**
 * memory.h — 内存管理模块 (动态分区)
 *
 * 8 条命令 (16分, PPT 第6页):
 *   alloc <size>         — 申请内存
 *   free_mem <addr>      — 释放内存
 *   show_mem             — 可视化内存布局
 *   compact              — 内存碎片紧缩
 *   mem_stat             — 内存使用统计
 *   set_alloc_algo <n>   — 切换分配算法 (0:FF 1:BF 2:WF)
 *   pgfault              — 模拟缺页中断
 *   swap_out <pid>       — 进程内存换出
 */

#ifndef MEMORY_H
#define MEMORY_H

#include "os_types.h"

/* 分配/释放 */
int  mem_alloc(SystemState *s, int size);
int  mem_free(SystemState *s, int start_addr);

/* 紧缩 */
void mem_compact(SystemState *s);

/* 显示 */
void mem_show(SystemState *s);
void mem_stat(SystemState *s);

/* 算法切换 */
void mem_set_algo(SystemState *s, int algo);

/* 缺页/换出 */
void mem_pgfault(SystemState *s);
int  mem_swap_out(SystemState *s, int pid);

/* 内部: 按当前算法在空闲链表中查找合适的块 */
FreeBlock* mem_find_free(SystemState *s, int size);

#endif /* MEMORY_H */
