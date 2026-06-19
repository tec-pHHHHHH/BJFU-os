#ifndef OS_TYPES_H
#define OS_TYPES_H

#include <pthread.h>
#include <time.h>

/* ================================================================
 * 一、全局常量
 * ================================================================ */

#define MAX_PROCESSES    256      /* 最大进程数 */
#define MAX_USERS        64       /* 最大用户数 */
#define MAX_CMD_LEN      512      /* 命令最大长度 */
#define MAX_NAME_LEN     32       /* 名称最大长度 */
#define MAX_PASSWORD_LEN 65       /* 密码哈希长度(64 hex + '\0') */
#define TOTAL_MEMORY     1024     /* 系统总内存(KB) */
#define MIN_ALLOC        8        /* 最小分配单位(KB) */

#define MLFQ_LEVELS      3        /* 多级反馈队列级数 */
#define MAX_PRIORITY     15       /* 最大优先级(lowest) */
#define MIN_PRIORITY     0        /* 最小优先级(highest) */

/* 持久化文件 */
#define STATE_FILE       "data/os_state.bin"
#define LOCK_FILE        "data/os_state.lock"
#define USER_DB_FILE     "data/users.db"

/* ================================================================
 * 二、进程状态枚举
 * ================================================================ */

typedef enum {
    STATE_RUNNING   = 0,   /* 正在CPU上执行 */
    STATE_READY     = 1,   /* 就绪，等待CPU */
    STATE_BLOCKED   = 2,   /* 阻塞，等待事件 */
    STATE_SUSPENDED = 3    /* 挂起，从调度中移除 */
} ProcessState;

/* ================================================================
 * 三、内存分配算法枚举
 * ================================================================ */

typedef enum {
    ALLOC_FIRST_FIT  = 0,  /* 首次适应 */
    ALLOC_BEST_FIT   = 1,  /* 最佳适应 */
    ALLOC_WORST_FIT  = 2   /* 最坏适应 */
} AllocAlgorithm;

/* ================================================================
 * 四、PCB — 进程控制块
 * ================================================================ */

typedef struct PCB {
    /* --- 基本信息 --- */
    int   pid;               /* 进程ID (唯一) */
    int   ppid;              /* 父进程ID */
    char  name[MAX_NAME_LEN];/* 进程名称 */
    char  owner[MAX_NAME_LEN];/* 所属用户名 */

    /* --- 状态与优先级 --- */
    ProcessState state;      /* 当前状态 */
    int   priority;          /* 优先级 0(最高) ~ 15(最低) */

    /* --- 内存信息 --- */
    int   mem_start;         /* 内存分配起始地址(KB) */
    int   mem_size;          /* 内存分配大小(KB) */

    /* --- 调度信息 --- */
    int   time_slice;        /* 剩余时间片 */
    int   total_ticks;       /* 累计执行的时钟节拍数 */
    int   queue_level;       /* 当前所在 MLFQ 队列级别 */

    /* --- 进程树指针 --- */
    struct PCB *parent;      /* 父进程 */
    struct PCB *first_child; /* 第一个子进程 */
    struct PCB *next_sibling;/* 下一个兄弟进程 */

    /* --- 进程总链表指针 --- */
    struct PCB *next;        /* 全局进程链表 next */

    /* --- 调度队列指针 (MLFQ 双向循环链表) --- */
    struct PCB *queue_next;  /* 队列后继 */
    struct PCB *queue_prev;  /* 队列前驱 */
} PCB;

/* ================================================================
 * 五、空闲内存分区块
 * ================================================================ */

typedef struct FreeBlock {
    int   start;             /* 起始地址(KB) */
    int   size;              /* 分区大小(KB) */
    struct FreeBlock *next;  /* 下一块 */
} FreeBlock;

/* ================================================================
 * 六、已分配内存块
 * ================================================================ */

typedef struct AllocBlock {
    int   start;             /* 起始地址(KB) */
    int   size;              /* 分配大小(KB) */
    int   pid;               /* 所属进程ID */
    struct AllocBlock *next; /* 下一块 */
} AllocBlock;

/* ================================================================
 * 七、多级反馈队列 (MLFQ) 配置
 *
 *    队列0: 优先级 0~3,  时间片 2 ticks (高优先级, 交互型)
 *    队列1: 优先级 4~7,  时间片 4 ticks (中等优先级)
 *    队列2: 优先级 8~15, 时间片 8 ticks (低优先级, 后台批处理)
 *
 *    调度策略: 高优先级队列优先; 同队列内时间片轮转(RR);
 *             进程用完时间片降级到下一队列; 饥饿老化提升。
 * ================================================================ */

/* 单级队列 — 双向循环链表 */
typedef struct {
    PCB  *head;              /* 队头 */
    PCB  *tail;              /* 队尾 */
    int   count;             /* 进程数 */
    int   quantum;           /* 该级时间片长度(tick) */
    int   prio_min;          /* 该级最小优先级 */
    int   prio_max;          /* 该级最大优先级 */
} MLFQ_Level;

/* 完整多级反馈队列 */
typedef struct {
    MLFQ_Level levels[MLFQ_LEVELS];
} MLFQ;

/* ================================================================
 * 八、用户信息
 * ================================================================ */

typedef struct User {
    char  username[MAX_NAME_LEN];
    char  password_hash[MAX_PASSWORD_LEN];  /* 简单哈希存储 */
    int   is_locked;         /* 账户是否被锁定(3次错误登录) */
    int   login_attempts;    /* 当前连续错误次数 */
    struct User *next;       /* 链表指针 */
} User;

/* ================================================================
 * 九、命令消息 (线程间通信)
 * ================================================================ */

typedef enum {
    CMD_EXIT     = 0,       /* 退出系统 */
    CMD_UNKNOWN  = -1       /* 未知命令 (实际命令用字符串) */
} CmdType;

typedef struct {
    char  text[MAX_CMD_LEN]; /* 原始命令字符串 */
    int   client_id;         /* 发起者标识(多实例场景) */
} Command;

/* 线程安全的消息队列节点 */
typedef struct CmdNode {
    Command           cmd;
    struct CmdNode   *next;
} CmdNode;

/* 线程安全的消息队列 */
typedef struct {
    CmdNode    *head;
    CmdNode    *tail;
    int         count;
    int         quit_flag;      /* 退出标志 */
    pthread_mutex_t mutex;      /* 互斥锁 */
    pthread_cond_t  cond;       /* 条件变量(队列空时消费者等待) */
} MessageQueue;

/* ================================================================
 * 十、系统全局状态 (单例)
 * ================================================================ */

typedef struct {
    /* --- 进程管理 --- */
    PCB     *process_list;      /* 所有进程链表 */
    int      process_count;     /* 当前进程数 */
    int      next_pid;          /* 下一个可用PID */
    PCB     *running;           /* 当前正在运行进程 */

    /* --- 调度器 --- */
    MLFQ     scheduler;         /* 多级反馈队列 */
    int      sched_running;     /* 调度器是否在自动运行 */
    int      sched_paused;      /* 调度器是否暂停 */
    int      system_clock;      /* 系统时钟(tick计数器) */

    /* --- 内存管理 --- */
    FreeBlock  *free_list;      /* 空闲分区链表(按地址排序) */
    AllocBlock *alloc_list;     /* 已分配分区链表 */
    int         total_mem;      /* 总内存(KB) */
    AllocAlgorithm alloc_algo;  /* 当前分配算法 */

    /* --- 用户管理 --- */
    User    *user_list;         /* 已注册用户链表 */
    char     current_user[MAX_NAME_LEN]; /* 当前登录用户 (空串=未登录) */

    /* --- 持久化 --- */
    int      file_locked;       /* 是否持有状态文件锁 */
    int      dirty;             /* 状态是否已修改(脏标志) */

    /* --- 多线程 --- */
    pthread_t  frontend_tid;    /* 前台线程ID */
    pthread_t  backend_tid;     /* 后台线程ID */
    pthread_t  sched_tid;       /* 调度器线程ID */
    MessageQueue cmd_queue;     /* 命令消息队列 */

    /* 调度器同步 */
    pthread_mutex_t sched_mutex; /* 调度器互斥锁 (暂停/恢复) */
    pthread_cond_t  sched_cond;  /* 调度器条件变量 */
    int             sched_speed; /* 每时间片模拟耗时(毫秒), 默认1000 */

    /* 多实例共享 */
    pthread_t       reload_tid;      /* 自动重载守护线程ID */
    int             reload_running;  /* 守护线程运行标志 */
    time_t          state_file_mtime;/* 状态文件最后修改时间 */
    int             autosave;        /* 是否自动保存(1=是, 0=否) */

    /* --- 日志/调试 --- */
    FILE    *log_fp;            /* 日志文件指针(NULL=不记录) */
} SystemState;

/* ================================================================
 * 十一、持久化文件二进制格式
 *
 * 文件结构:
 *   [Header] [User Table] [PCB Table] [MLFQ State] [Memory State]
 *
 * Header 包含各段偏移量, 方便按需读取。
 * ================================================================ */

/* 魔数, 用于识别合法状态文件 */
#define OS_MAGIC  0x4F533230  /* "OS20" = OS 2025 */

typedef struct {
    int   magic;               /* 魔数 OS_MAGIC */
    int   version;             /* 文件格式版本 */
    int   next_pid;            /* 下一个可用PID */
    int   system_clock;        /* 系统时钟 */
    int   sched_running;       /* 调度器运行标志 */
    int   alloc_algo;          /* 当前分配算法 */

    /* 各段计数 */
    int   user_count;          /* 用户数 */
    int   pcb_count;           /* 进程数 */
    int   free_block_count;    /* 空闲块数 */
    int   alloc_block_count;   /* 已分配块数 */

    /* 各段在文件中的偏移量(字节) */
    long  user_offset;
    long  pcb_offset;
    long  mlfq_offset;
    long  free_offset;
    long  alloc_offset;
} PersistHeader;

#endif /* OS_TYPES_H */
