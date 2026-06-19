#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include "include/os_types.h"
#include "include/user.h"
#include "include/process.h"
#include "include/scheduler.h"
#include "include/memory.h"
#include "include/persistence.h"
#include "include/concurrency.h"
#include "include/visualization.h"

/* ================================================================
 * Global system state (singleton)
 * ================================================================ */

SystemState sys;

/* ================================================================
 * Message queue operations
 * ================================================================ */

void mq_init(MessageQueue *q) {
    q->head = q->tail = NULL;
    q->count = 0;
    q->quit_flag = 0;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
}

void mq_put(MessageQueue *q, const char *cmd_text, int client_id) {
    CmdNode *node = malloc(sizeof(CmdNode));
    if (!node) { perror("malloc"); return; }
    strncpy(node->cmd.text, cmd_text, MAX_CMD_LEN - 1);
    node->cmd.text[MAX_CMD_LEN - 1] = '\0';
    node->cmd.client_id = client_id;
    node->next = NULL;

    pthread_mutex_lock(&q->mutex);
    if (q->tail) {
        q->tail->next = node;
    } else {
        q->head = node;
    }
    q->tail = node;
    q->count++;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

/* blocking get: returns 0 on success, -1 on quit */
int mq_get(MessageQueue *q, Command *out) {
    pthread_mutex_lock(&q->mutex);
    while (q->head == NULL && !q->quit_flag) {
        pthread_cond_wait(&q->cond, &q->mutex);
    }
    if (q->quit_flag && q->head == NULL) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }
    CmdNode *node = q->head;
    q->head = node->next;
    if (!q->head) q->tail = NULL;
    q->count--;
    *out = node->cmd;
    free(node);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

void mq_signal_quit(MessageQueue *q) {
    pthread_mutex_lock(&q->mutex);
    q->quit_flag = 1;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

/* ================================================================
 * MLFQ init — 3 levels:
 *   Q0 — prio 0~3,  quantum=2 (interactive)
 *   Q1 — prio 4~7,  quantum=4 (normal)
 *   Q2 — prio 8~15, quantum=8 (batch)
 * ================================================================ */

void init_mlfq(MLFQ *mlfq) {
    mlfq->levels[0].quantum  = 2;
    mlfq->levels[0].prio_min = 0;
    mlfq->levels[0].prio_max = 3;
    mlfq->levels[0].head     = NULL;
    mlfq->levels[0].tail     = NULL;
    mlfq->levels[0].count    = 0;

    mlfq->levels[1].quantum  = 4;
    mlfq->levels[1].prio_min = 4;
    mlfq->levels[1].prio_max = 7;
    mlfq->levels[1].head     = NULL;
    mlfq->levels[1].tail     = NULL;
    mlfq->levels[1].count    = 0;

    mlfq->levels[2].quantum  = 8;
    mlfq->levels[2].prio_min = 8;
    mlfq->levels[2].prio_max = 15;
    mlfq->levels[2].head     = NULL;
    mlfq->levels[2].tail     = NULL;
    mlfq->levels[2].count    = 0;
}

/* ================================================================
 * System init
 * ================================================================ */

void init_system(SystemState *s) {
    memset(s, 0, sizeof(*s));

    /* process management */
    s->process_list   = NULL;
    s->process_count  = 0;
    s->next_pid       = 1;     /* PID=1 reserved for init */
    s->running        = NULL;

    /* scheduler */
    init_mlfq(&s->scheduler);
    s->sched_running  = 0;
    s->sched_paused   = 0;
    s->system_clock   = 0;

    /* memory: one large free block */
    s->free_list = malloc(sizeof(FreeBlock));
    s->free_list->start = 0;
    s->free_list->size  = TOTAL_MEMORY;
    s->free_list->next  = NULL;
    s->alloc_list       = NULL;
    s->total_mem        = TOTAL_MEMORY;
    s->alloc_algo       = ALLOC_FIRST_FIT;

    /* user management */
    s->user_list  = NULL;
    s->current_user[0] = '\0';

    /* persistence */
    s->file_locked = 0;
    s->dirty       = 0;

    /* message queue */
    mq_init(&s->cmd_queue);

    /* scheduler thread */
    s->sched_tid    = 0;
    s->sched_speed  = 1000;
    s->sched_running = 0;
    s->sched_paused  = 0;

    /* multi-instance sharing */
    s->reload_tid      = 0;
    s->reload_running  = 0;
    s->state_file_mtime = 0;
    s->autosave        = 0;

    /* logging */
    s->log_fp = NULL;
}

/* ================================================================
 * Frontend thread: read stdin, push to message queue
 * ================================================================ */

void *frontend_thread(void *arg) {
    SystemState *s = (SystemState *)arg;
    char line[MAX_CMD_LEN];

    printf("============================================\n");
    printf("  OS Core Simulator v1.0\n");
    printf("  Type 'help' for commands, 'exit' to quit\n");
    printf("============================================\n\n");

    while (1) {
        if (s->current_user[0]) {
            printf("[%s@os-sim]$ ", s->current_user);
        } else {
            printf("[guest@os-sim]$ ");
        }
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            mq_put(&s->cmd_queue, "exit", 0);
            break;
        }

        line[strcspn(line, "\r\n")] = '\0';
        if (strlen(line) == 0) continue;

        mq_put(&s->cmd_queue, line, 0);

        if (strcmp(line, "exit") == 0) break;
    }
    return NULL;
}

/* ================================================================
 * Backend thread: dequeue commands, parse and dispatch
 * ================================================================ */

void *backend_thread(void *arg) {
    SystemState *s = (SystemState *)arg;
    Command cmd;

    while (mq_get(&s->cmd_queue, &cmd) == 0) {
        char *line = cmd.text;

        while (*line == ' ' || *line == '\t') line++;
        if (*line == '\0') continue;

        char cmd_name[64] = {0};
        sscanf(line, "%63s", cmd_name);

        /* ================================================
         * System commands (no login required)
         * ================================================ */

        if (strcmp(cmd_name, "exit") == 0) {
            printf("[System] Shutting down...\n");

            concurrency_stop_daemon(s);

            if (s->sched_running) {
                s->sched_running = 0;
                pthread_mutex_lock(&s->sched_mutex);
                s->sched_paused = 0;
                pthread_cond_signal(&s->sched_cond);
                pthread_mutex_unlock(&s->sched_mutex);
                pthread_join(s->sched_tid, NULL);
            }
            break;
        }

        if (strcmp(cmd_name, "help") == 0) {
            printf("\n========== Command List ==========\n");
            printf("  register <user> <pass>   -- Register new user\n");
            printf("  login    <user> <pass>   -- Login\n");
            printf("  logout                   -- Logout\n");
            printf("  help                     -- Show this help\n");
            printf("  exit                     -- Exit simulator\n");
            printf("--- Persistence ------------------\n");
            printf("  save                     -- Save state to file\n");
            printf("  load                     -- Load state from file\n");
            printf("  autosave on/off          -- Toggle auto-save\n");
            printf("--- Scheduler --------------------\n");
            printf("  start_sched              -- Start auto scheduler\n");
            printf("  stop_sched               -- Pause scheduler\n");
            printf("  restart_sched            -- Resume scheduler\n");
            printf("  step                     -- Single-step execution\n");
            printf("--- Memory Management ------------\n");
            printf("  alloc      <size:KB>     -- Allocate memory\n");
            printf("  free_mem   <addr>        -- Free memory\n");
            printf("  show_mem                 -- Show memory layout\n");
            printf("  compact                  -- Defragment memory\n");
            printf("  mem_stat                 -- Memory statistics\n");
            printf("  set_alloc_algo <0/1/2>   -- Switch FF/BF/WF\n");
            printf("  pgfault                  -- Simulate page fault\n");
            printf("  swap_out   <pid>         -- Swap out process memory\n");
            printf("--- Process Management -----------\n");
            printf("  create_pcb <name> <prio> -- Create process\n");
            printf("  kill_pcb   <pid>         -- Kill process\n");
            printf("  block_pcb  <pid>         -- Block process\n");
            printf("  wakeup_pcb <pid>         -- Wake process\n");
            printf("  show_pcb   <pid>         -- Show process details\n");
            printf("  list_pcb                 -- List all processes\n");
            printf("  ptree                    -- Process tree\n");
            printf("  suspend    <pid>         -- Suspend process\n");
            printf("  resume     <pid>         -- Resume process\n");
            printf("  renice     <pid> <prio>  -- Change priority\n");
            printf("--- Visualization ----------------\n");
            printf("  overview                 -- Global overview\n");
            printf("================================\n\n");
            continue;
        }

        /* register */
        if (strcmp(cmd_name, "register") == 0) {
            char uname[MAX_NAME_LEN] = {0};
            char pwd[MAX_NAME_LEN]   = {0};
            if (sscanf(line, "%*s %31s %31s", uname, pwd) < 2) {
                printf("[Error] Usage: register <user> <password>\n");
                continue;
            }
            int ret = user_register(s, uname, pwd);
            switch (ret) {
                case USER_OK:         printf("[Register] User '%s' created.\n", uname); break;
                case USER_EXISTS:     printf("[Register] FAIL: user '%s' already exists.\n", uname); break;
                case USER_FULL:       printf("[Register] FAIL: user limit reached.\n"); break;
                case USER_EMPTY_NAME: printf("[Register] FAIL: invalid username (alphanumeric + underscore only).\n"); break;
                case USER_EMPTY_PWD:  printf("[Register] FAIL: password cannot be empty.\n"); break;
                default:              printf("[Register] FAIL: unknown error (%d).\n", ret); break;
            }
            continue;
        }

        /* login */
        if (strcmp(cmd_name, "login") == 0) {
            char uname[MAX_NAME_LEN] = {0};
            char pwd[MAX_NAME_LEN]   = {0};
            if (sscanf(line, "%*s %31s %31s", uname, pwd) < 2) {
                printf("[Error] Usage: login <user> <password>\n");
                continue;
            }
            int ret = user_login(s, uname, pwd);
            switch (ret) {
                case USER_OK:
                    concurrency_start_daemon(s);
                    break;
                case USER_NOT_FOUND: printf("[Login] FAIL: user '%s' not found.\n", uname); break;
                case USER_WRONG_PWD: printf("[Login] FAIL: wrong password.\n"); break;
                case USER_LOCKED:    printf("[Login] FAIL: account '%s' is locked.\n", uname); break;
                default:             printf("[Login] FAIL: unknown error (%d).\n", ret); break;
            }
            continue;
        }

        /* logout */
        if (strcmp(cmd_name, "logout") == 0) {
            user_logout(s);
            continue;
        }

        /* ================================================
         * Commands requiring login
         * ================================================ */

        if (s->current_user[0] == '\0') {
            printf("[Hint] Please login first (login) or register (register).\n");
            continue;
        }

        /* ================================================
         * Process management commands (10)
         * ================================================ */

        /* create_pcb <name> <priority> */
        if (strcmp(cmd_name, "create_pcb") == 0) {
            char pname[MAX_NAME_LEN] = {0};
            int prio = 5;
            if (sscanf(line, "%*s %31s %d", pname, &prio) < 1) {
                printf("[Error] Usage: create_pcb <name> [priority:0-15]\n");
                continue;
            }
            int ret = pcb_create(s, pname, prio);
            if (ret == PROC_FULL) printf("[Error] Process table full.\n");
            else if (ret == PROC_INVALID_PID) printf("[Error] Invalid arguments.\n");
            continue;
        }

        /* kill_pcb <pid> */
        if (strcmp(cmd_name, "kill_pcb") == 0) {
            int pid = 0;
            if (sscanf(line, "%*s %d", &pid) < 1) {
                printf("[Error] Usage: kill_pcb <pid>\n");
                continue;
            }
            int ret = pcb_kill(s, pid);
            if (ret == PROC_NOT_FOUND) printf("[Error] Process PID=%d not found.\n", pid);
            else if (ret == PROC_NO_PERM) printf("[Error] Permission denied.\n");
            continue;
        }

        /* block_pcb <pid> */
        if (strcmp(cmd_name, "block_pcb") == 0) {
            int pid = 0;
            if (sscanf(line, "%*s %d", &pid) < 1) {
                printf("[Error] Usage: block_pcb <pid>\n");
                continue;
            }
            int ret = pcb_block(s, pid);
            if (ret == PROC_NOT_FOUND) printf("[Error] Process PID=%d not found.\n", pid);
            continue;
        }

        /* wakeup_pcb <pid> */
        if (strcmp(cmd_name, "wakeup_pcb") == 0) {
            int pid = 0;
            if (sscanf(line, "%*s %d", &pid) < 1) {
                printf("[Error] Usage: wakeup_pcb <pid>\n");
                continue;
            }
            int ret = pcb_wakeup(s, pid);
            if (ret == PROC_NOT_FOUND) printf("[Error] Process PID=%d not found.\n", pid);
            else if (ret == PROC_BAD_STATE) printf("[Error] Process is not in BLOCKED state.\n");
            continue;
        }

        /* show_pcb <pid> */
        if (strcmp(cmd_name, "show_pcb") == 0) {
            int pid = 0;
            if (sscanf(line, "%*s %d", &pid) < 1) {
                printf("[Error] Usage: show_pcb <pid>\n");
                continue;
            }
            pcb_show(s, pid);
            continue;
        }

        /* list_pcb */
        if (strcmp(cmd_name, "list_pcb") == 0) {
            pcb_list(s);
            continue;
        }

        /* ptree */
        if (strcmp(cmd_name, "ptree") == 0) {
            pcb_ptree(s);
            continue;
        }

        /* suspend <pid> */
        if (strcmp(cmd_name, "suspend") == 0) {
            int pid = 0;
            if (sscanf(line, "%*s %d", &pid) < 1) {
                printf("[Error] Usage: suspend <pid>\n");
                continue;
            }
            int ret = pcb_suspend(s, pid);
            if (ret == PROC_NOT_FOUND) printf("[Error] Process PID=%d not found.\n", pid);
            continue;
        }

        /* resume <pid> */
        if (strcmp(cmd_name, "resume") == 0) {
            int pid = 0;
            if (sscanf(line, "%*s %d", &pid) < 1) {
                printf("[Error] Usage: resume <pid>\n");
                continue;
            }
            int ret = pcb_resume(s, pid);
            if (ret == PROC_NOT_FOUND) printf("[Error] Process PID=%d not found.\n", pid);
            else if (ret == PROC_BAD_STATE) printf("[Error] Process is not in SUSPENDED state.\n");
            continue;
        }

        /* renice <pid> <new_priority> */
        if (strcmp(cmd_name, "renice") == 0) {
            int pid = 0, newp = 0;
            if (sscanf(line, "%*s %d %d", &pid, &newp) < 2) {
                printf("[Error] Usage: renice <pid> <new_priority:0-15>\n");
                continue;
            }
            int ret = pcb_renice(s, pid, newp);
            if (ret == PROC_NOT_FOUND) printf("[Error] Process PID=%d not found.\n", pid);
            else if (ret == PROC_INVALID_PID) printf("[Error] Invalid priority (0-15).\n");
            continue;
        }

        /* ================================================
         * Scheduler commands (4)
         * ================================================ */

        if (strcmp(cmd_name, "start_sched") == 0) {
            scheduler_start(s);
            continue;
        }

        if (strcmp(cmd_name, "stop_sched") == 0) {
            scheduler_stop(s);
            continue;
        }

        if (strcmp(cmd_name, "restart_sched") == 0) {
            scheduler_restart(s);
            continue;
        }

        if (strcmp(cmd_name, "step") == 0) {
            scheduler_step(s);
            continue;
        }

        /* ================================================
         * Memory management commands (8)
         * ================================================ */

        if (strcmp(cmd_name, "alloc") == 0) {
            int size = 0;
            if (sscanf(line, "%*s %d", &size) < 1) {
                printf("[Error] Usage: alloc <size:KB>\n");
                continue;
            }
            mem_alloc(s, size);
            continue;
        }

        if (strcmp(cmd_name, "free_mem") == 0) {
            int addr = 0;
            if (sscanf(line, "%*s %d", &addr) < 1) {
                printf("[Error] Usage: free_mem <start_addr>\n");
                continue;
            }
            mem_free(s, addr);
            continue;
        }

        if (strcmp(cmd_name, "show_mem") == 0) {
            mem_show(s);
            continue;
        }

        if (strcmp(cmd_name, "compact") == 0) {
            mem_compact(s);
            continue;
        }

        if (strcmp(cmd_name, "mem_stat") == 0) {
            mem_stat(s);
            continue;
        }

        if (strcmp(cmd_name, "set_alloc_algo") == 0) {
            int algo = 0;
            if (sscanf(line, "%*s %d", &algo) < 1) {
                printf("[Error] Usage: set_alloc_algo <0:FF|1:BF|2:WF>\n");
                continue;
            }
            mem_set_algo(s, algo);
            continue;
        }

        if (strcmp(cmd_name, "pgfault") == 0) {
            mem_pgfault(s);
            continue;
        }

        if (strcmp(cmd_name, "swap_out") == 0) {
            int pid = 0;
            if (sscanf(line, "%*s %d", &pid) < 1) {
                printf("[Error] Usage: swap_out <pid>\n");
                continue;
            }
            mem_swap_out(s, pid);
            continue;
        }

        /* ================================================
         * Persistence commands
         * ================================================ */

        if (strcmp(cmd_name, "save") == 0) {
            persistence_save(s);
            continue;
        }

        if (strcmp(cmd_name, "load") == 0) {
            persistence_load(s);
            continue;
        }

        if (strcmp(cmd_name, "autosave") == 0) {
            char arg[8] = {0};
            sscanf(line, "%*s %7s", arg);
            if (strcmp(arg, "on") == 0) {
                s->autosave = 1;
                printf("[Autosave] ON — state will be written to disk on changes.\n");
            } else if (strcmp(arg, "off") == 0) {
                s->autosave = 0;
                printf("[Autosave] OFF.\n");
            } else {
                printf("[Autosave] Current: %s. Usage: autosave on|off\n",
                       s->autosave ? "ON" : "OFF");
            }
            continue;
        }

        /* ================================================
         * Visualization command
         * ================================================ */

        if (strcmp(cmd_name, "overview") == 0) {
            overview(s);
            continue;
        }

        /* unknown command */
        printf("[Error] Unknown command: '%s'. Type 'help' for available commands.\n", cmd_name);

        /* auto-save after state changes */
        concurrency_autosave(s);
    }
    return NULL;
}

/* ================================================================
 * main entry
 * ================================================================ */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    printf("[Init] Starting OS Core Simulator...\n");

    /* 1. Init system state */
    init_system(&sys);

    /* 2. Load user database */
    user_load_from_file(&sys);

    /* 3. Auto-load persistence state if available */
    persistence_load(&sys);

    /* 4. Start backend thread */
    if (pthread_create(&sys.backend_tid, NULL, backend_thread, &sys) != 0) {
        perror("pthread_create (backend)");
        exit(1);
    }

    /* 5. Frontend runs on main thread */
    frontend_thread(&sys);

    /* 6. Wait for backend */
    pthread_join(sys.backend_tid, NULL);

    /* 7. Cleanup */
    printf("[Info] Simulator exited. Total clock: %d ticks\n", sys.system_clock);
    return 0;
}
