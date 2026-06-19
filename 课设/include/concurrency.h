/**
 * concurrency.h — 多实例并发共享模块
 *
 * PPT 第8页 (18分):
 *   基础架构 (10分): 前后台线程分离 + 消息队列 — 已实现
 *   多实例共享 (8分): 文件锁 + 自动重载守护线程
 *
 *   验收: A终端 create_pcb → B终端 overview 立即可见
 */

#ifndef CONCURRENCY_H
#define CONCURRENCY_H

#include "os_types.h"

/* 重载守护线程轮询间隔 (秒) */
#define RELOAD_INTERVAL_SEC  2

/**
 * 启动自动重载守护线程
 * 登录成功后调用, 持续监控状态文件变化
 */
int concurrency_start_daemon(SystemState *s);

/**
 * 停止守护线程 (退出时调用)
 */
void concurrency_stop_daemon(SystemState *s);

/**
 * 获取文件写锁 (跨实例互斥)
 * 返回: 0成功, -1被其他实例占用
 */
int concurrency_lock(void);

/**
 * 释放文件写锁
 */
void concurrency_unlock(void);

/**
 * 自动保存: 若 autosave 开启且状态已修改, 加锁保存
 * 应在每条修改状态的命令后调用
 */
void concurrency_autosave(SystemState *s);

#endif /* CONCURRENCY_H */
