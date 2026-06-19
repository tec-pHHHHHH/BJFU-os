/**
 * persistence.h — 状态持久化模块
 *
 * 2 条命令 (20分, PPT 第7页):
 *   save — 全量快照保存到二进制文件 (10分)
 *   load — 从二进制文件恢复全部状态 (10分)
 *
 * 核心验收: 恢复后队列顺序、剩余时间片、已执行CPU时间必须精确一致
 * 文件格式: 二进制 (硬性要求, 文本文件扣10分)
 */

#ifndef PERSISTENCE_H
#define PERSISTENCE_H

#include "os_types.h"

/**
 * 保存当前系统状态到 STATE_FILE
 * 返回: 0成功, -1失败
 */
int persistence_save(SystemState *s);

/**
 * 从 STATE_FILE 加载状态到系统
 * 返回: 1成功加载, 0文件不存在(冷启动), -1格式错误
 */
int persistence_load(SystemState *s);

#endif /* PERSISTENCE_H */
