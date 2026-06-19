/**
 * user.h — 用户管理模块
 *
 * 功能: 注册、登录、注销, 密码哈希存储, 3次错误锁定, 用户文件持久化
 */

#ifndef USER_H
#define USER_H

#include "os_types.h"

/* 错误码 */
#define USER_OK           0   /* 操作成功 */
#define USER_NOT_FOUND   -1   /* 用户不存在 */
#define USER_WRONG_PWD   -2   /* 密码错误 */
#define USER_LOCKED      -3   /* 账户已锁定 */
#define USER_EXISTS      -4   /* 用户名已存在 */
#define USER_FULL        -5   /* 用户数已达上限 */
#define USER_EMPTY_NAME  -6   /* 用户名为空 */
#define USER_EMPTY_PWD   -7   /* 密码为空 */

/**
 * 密码哈希 — 简单 SHA-256 风格摘要 (无外部依赖)
 * 输入: password 原始密码
 * 输出: hash_out  64字符十六进制哈希串 + '\0'
 */
void user_hash_password(const char *password, char *hash_out);

/**
 * 查找用户
 * 返回: User指针, 未找到返回 NULL
 */
User* user_find(SystemState *s, const char *username);

/**
 * 注册新用户
 * 返回: USER_OK / USER_EXISTS / USER_FULL / USER_EMPTY_NAME / USER_EMPTY_PWD
 */
int user_register(SystemState *s, const char *username, const char *password);

/**
 * 用户登录
 * 返回: USER_OK / USER_NOT_FOUND / USER_WRONG_PWD / USER_LOCKED
 * 成功后将 current_user 设为该用户名
 */
int user_login(SystemState *s, const char *username, const char *password);

/**
 * 用户登出 — 清空 current_user
 */
void user_logout(SystemState *s);

/**
 * 保存用户数据库到文件
 */
int user_save_to_file(SystemState *s);

/**
 * 从文件加载用户数据库
 */
int user_load_from_file(SystemState *s);

#endif /* USER_H */
