/**
 * process.c — Process management module
 *
 * 10 commands: create_pcb / kill_pcb / block_pcb / wakeup_pcb / show_pcb
 *              list_pcb / ptree / suspend / resume / renice
 *
 * Design:
 *   - init process (PID=1) auto-created on first create_pcb, cannot be killed
 *   - each process belongs to one user (owner), permission-checked
 *   - kill recursively cleans children + reclaims memory
 *   - MLFQ enqueue auto-selects queue level by priority
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "include/os_types.h"
#include "include/process.h"

/* ================================================================
 * Helper: state -> string
 * ================================================================ */

const char* pcb_state_string(ProcessState state) {
    switch (state) {
        case STATE_RUNNING:   return "RUNNING";
        case STATE_READY:     return "READY";
        case STATE_BLOCKED:   return "BLOCKED";
        case STATE_SUSPENDED: return "SUSPENDED";
        default:              return "UNKNOWN";
    }
}

/* ================================================================
 * MLFQ internal: map priority to queue level
 * ================================================================ */

static int priority_to_queue(int priority) {
    if (priority <= 3)  return 0;
    if (priority <= 7)  return 1;
    return 2;
}

/* ================================================================
 * MLFQ internal: enqueue process to ready queue (tail)
 * ================================================================ */

void pcb_enqueue_ready(SystemState *s, PCB *p) {
    int level = priority_to_queue(p->priority);
    MLFQ_Level *q = &s->scheduler.levels[level];

    p->queue_level = level;
    p->state       = STATE_READY;
    p->queue_next  = NULL;
    p->queue_prev  = NULL;

    if (!q->head) {
        q->head = q->tail = p;
        p->queue_next = p->queue_prev = p;
    } else {
        p->queue_prev = q->tail;
        p->queue_next = q->head;
        q->tail->queue_next = p;
        q->head->queue_prev = p;
        q->tail = p;
    }
    q->count++;
}

/* ================================================================
 * MLFQ internal: dequeue process from its queue
 * ================================================================ */

void pcb_dequeue(SystemState *s, PCB *p) {
    int level = p->queue_level;
    MLFQ_Level *q = &s->scheduler.levels[level];

    if (!q->head || q->count == 0) return;

    if (q->head == p && q->tail == p && q->count == 1) {
        q->head = q->tail = NULL;
    } else {
        p->queue_prev->queue_next = p->queue_next;
        p->queue_next->queue_prev = p->queue_prev;
        if (q->head == p) q->head = p->queue_next;
        if (q->tail == p) q->tail = p->queue_prev;
    }
    p->queue_next = p->queue_prev = NULL;
    q->count--;
}

/* ================================================================
 * Find process by PID
 * ================================================================ */

PCB* pcb_find(SystemState *s, int pid) {
    PCB *p = s->process_list;
    while (p) {
        if (p->pid == pid) return p;
        p = p->next;
    }
    return NULL;
}

/* ================================================================
 * create_pcb <name> <priority>
 * ================================================================ */

int pcb_create(SystemState *s, const char *name, int priority) {
    if (!name || strlen(name) == 0) return PROC_INVALID_PID;
    if (priority < MIN_PRIORITY || priority > MAX_PRIORITY) return PROC_INVALID_PID;
    if (s->process_count >= MAX_PROCESSES) return PROC_FULL;

    /* auto-create init if needed */
    if (s->process_list == NULL) {
        PCB *init = calloc(1, sizeof(PCB));
        if (!init) return PROC_FULL;

        init->pid       = s->next_pid++;
        init->ppid      = 0;
        strncpy(init->name, "init", MAX_NAME_LEN - 1);
        strncpy(init->owner, "system", MAX_NAME_LEN - 1);
        init->state     = STATE_READY;
        init->priority  = 0;
        init->mem_start = 0;
        init->mem_size  = 64;
        init->time_slice = s->scheduler.levels[0].quantum;

        init->next = NULL;
        s->process_list = init;
        s->process_count++;

        pcb_enqueue_ready(s, init);

        if (s->free_list && s->free_list->size >= 64) {
            AllocBlock *ab = malloc(sizeof(AllocBlock));
            ab->start = 0;
            ab->size  = 64;
            ab->pid   = init->pid;
            ab->next  = s->alloc_list;
            s->alloc_list = ab;

            s->free_list->start += 64;
            s->free_list->size  -= 64;
            if (s->free_list->size == 0) {
                free(s->free_list);
                s->free_list = NULL;
            }
        }

        printf("[Create] init (PID=1, prio=0, mem=64KB) auto-created.\n");
    }

    /* create target process */
    PCB *p = calloc(1, sizeof(PCB));
    if (!p) return PROC_FULL;

    p->pid       = s->next_pid++;
    p->ppid      = s->running ? s->running->pid : 1;
    strncpy(p->name, name, MAX_NAME_LEN - 1);
    strncpy(p->owner, s->current_user, MAX_NAME_LEN - 1);
    p->state     = STATE_READY;
    p->priority  = priority;
    p->mem_start = -1;
    p->mem_size  = 0;
    p->time_slice = s->scheduler.levels[priority_to_queue(priority)].quantum;

    PCB *parent = pcb_find(s, p->ppid);
    if (parent) {
        p->parent = parent;
        p->next_sibling = parent->first_child;
        parent->first_child = p;
    }

    p->next = s->process_list;
    s->process_list = p;
    s->process_count++;

    pcb_enqueue_ready(s, p);

    s->dirty = 1;
    printf("[Create] %s (PID=%d, prio=%d) - parent PID=%d\n",
           p->name, p->pid, p->priority, p->ppid);
    return PROC_OK;
}

/* ================================================================
 * kill_pcb <pid> — recursively kill process and all children
 * ================================================================ */

static void pcb_kill_recursive(SystemState *s, PCB *p) {
    if (!p) return;

    PCB *child = p->first_child;
    while (child) {
        PCB *next = child->next_sibling;
        pcb_kill_recursive(s, child);
        child = next;
    }

    if (p->state == STATE_READY || p->state == STATE_RUNNING) {
        pcb_dequeue(s, p);
    }

    if (p->mem_size > 0 && p->mem_start >= 0) {
        AllocBlock *prev = NULL, *ab = s->alloc_list;
        while (ab) {
            if (ab->pid == p->pid) {
                if (prev) prev->next = ab->next;
                else      s->alloc_list = ab->next;

                FreeBlock *fb = malloc(sizeof(FreeBlock));
                fb->start = ab->start;
                fb->size  = ab->size;
                FreeBlock **fp = &s->free_list;
                while (*fp && (*fp)->start < fb->start) fp = &(*fp)->next;
                fb->next = *fp;
                *fp = fb;

                free(ab);
                break;
            }
            prev = ab;
            ab = ab->next;
        }
    }

    if (p->parent) {
        PCB **cp = &p->parent->first_child;
        while (*cp) {
            if (*cp == p) { *cp = p->next_sibling; break; }
            cp = &(*cp)->next_sibling;
        }
    }

    PCB **pp = &s->process_list;
    while (*pp) {
        if (*pp == p) { *pp = p->next; break; }
        pp = &(*pp)->next;
    }

    if (s->running == p) s->running = NULL;

    s->process_count--;
    free(p);
}

int pcb_kill(SystemState *s, int pid) {
    if (pid <= 0) return PROC_INVALID_PID;

    PCB *p = pcb_find(s, pid);
    if (!p) return PROC_NOT_FOUND;

    if (strlen(s->current_user) > 0 &&
        strcmp(p->owner, s->current_user) != 0 &&
        strcmp(p->owner, "system") != 0) {
        return PROC_NO_PERM;
    }

    if (pid == 1) {
        printf("[Error] Cannot kill init process.\n");
        return PROC_BAD_STATE;
    }

    char pname[MAX_NAME_LEN];
    strncpy(pname, p->name, MAX_NAME_LEN - 1);
    pname[MAX_NAME_LEN - 1] = '\0';

    pcb_kill_recursive(s, p);
    s->dirty = 1;

    printf("[Kill] Process %s (PID=%d) and children terminated, resources reclaimed.\n", pname, pid);
    return PROC_OK;
}

/* ================================================================
 * block_pcb <pid> — block process
 * ================================================================ */

int pcb_block(SystemState *s, int pid) {
    PCB *p = pcb_find(s, pid);
    if (!p) return PROC_NOT_FOUND;

    if (p->state == STATE_BLOCKED) {
        printf("[Hint] Process %d is already BLOCKED.\n", pid);
        return PROC_OK;
    }
    if (p->state == STATE_SUSPENDED) return PROC_BAD_STATE;

    if (p->state == STATE_READY || p->state == STATE_RUNNING) {
        pcb_dequeue(s, p);
    }
    if (s->running == p) s->running = NULL;

    p->state = STATE_BLOCKED;
    s->dirty = 1;

    printf("[Block] Process %s (PID=%d) -> BLOCKED.\n", p->name, pid);
    return PROC_OK;
}

/* ================================================================
 * wakeup_pcb <pid> — wake up blocked process
 * ================================================================ */

int pcb_wakeup(SystemState *s, int pid) {
    PCB *p = pcb_find(s, pid);
    if (!p) return PROC_NOT_FOUND;

    if (p->state != STATE_BLOCKED) {
        printf("[Hint] Process %d is not BLOCKED (current: %s).\n",
               pid, pcb_state_string(p->state));
        return PROC_BAD_STATE;
    }

    pcb_enqueue_ready(s, p);
    s->dirty = 1;

    printf("[Wakeup] Process %s (PID=%d) -> READY, enqueued to Q%d.\n",
           p->name, pid, p->queue_level);
    return PROC_OK;
}

/* ================================================================
 * suspend <pid> — suspend process
 * ================================================================ */

int pcb_suspend(SystemState *s, int pid) {
    PCB *p = pcb_find(s, pid);
    if (!p) return PROC_NOT_FOUND;

    if (p->state == STATE_SUSPENDED) {
        printf("[Hint] Process %d is already SUSPENDED.\n", pid);
        return PROC_OK;
    }

    if (p->state == STATE_READY || p->state == STATE_RUNNING) {
        pcb_dequeue(s, p);
    }
    if (s->running == p) s->running = NULL;

    p->state = STATE_SUSPENDED;
    s->dirty = 1;

    printf("[Suspend] Process %s (PID=%d) -> SUSPENDED (removed from scheduler).\n", p->name, pid);
    return PROC_OK;
}

/* ================================================================
 * resume <pid> — resume suspended process
 * ================================================================ */

int pcb_resume(SystemState *s, int pid) {
    PCB *p = pcb_find(s, pid);
    if (!p) return PROC_NOT_FOUND;

    if (p->state != STATE_SUSPENDED) {
        printf("[Hint] Process %d is not SUSPENDED (current: %s).\n",
               pid, pcb_state_string(p->state));
        return PROC_BAD_STATE;
    }

    pcb_enqueue_ready(s, p);
    s->dirty = 1;

    printf("[Resume] Process %s (PID=%d) -> READY, re-entered scheduler.\n", p->name, pid);
    return PROC_OK;
}

/* ================================================================
 * renice <pid> <new_priority> — change process priority
 * ================================================================ */

int pcb_renice(SystemState *s, int pid, int new_priority) {
    if (new_priority < MIN_PRIORITY || new_priority > MAX_PRIORITY)
        return PROC_INVALID_PID;

    PCB *p = pcb_find(s, pid);
    if (!p) return PROC_NOT_FOUND;

    int old_prio = p->priority;
    p->priority = new_priority;

    if (p->state == STATE_READY || p->state == STATE_RUNNING) {
        pcb_dequeue(s, p);
        pcb_enqueue_ready(s, p);
    }

    s->dirty = 1;
    printf("[Renice] Process %s (PID=%d): priority %d -> %d (Q%d->Q%d).\n",
           p->name, pid, old_prio, new_priority,
           priority_to_queue(old_prio), priority_to_queue(new_priority));
    return PROC_OK;
}

/* ================================================================
 * show_pcb <pid> — display single process details
 * ================================================================ */

void pcb_show(SystemState *s, int pid) {
    PCB *p = pcb_find(s, pid);
    if (!p) {
        printf("[Error] Process PID=%d not found.\n", pid);
        return;
    }

    printf("\n+------- PCB Details ---------+\n");
    printf("| PID:        %-16d |\n", p->pid);
    printf("| Name:       %-16s |\n", p->name);
    printf("| State:      %-16s |\n", pcb_state_string(p->state));
    printf("| Priority:   %-16d |\n", p->priority);
    printf("| Owner:      %-16s |\n", p->owner);
    printf("| Parent PID: %-16d |\n", p->ppid);
    printf("| Mem Start:  %-16d KB|\n", p->mem_start);
    printf("| Mem Size:   %-16d KB|\n", p->mem_size);
    printf("| Time Slice: %-16d |\n", p->time_slice);
    printf("| Total Ticks:%d%-9d ticks|\n", p->total_ticks > 999999 ? 0 : 16 - (p->total_ticks > 9999 ? 5 : p->total_ticks > 999 ? 4 : p->total_ticks > 99 ? 3 : p->total_ticks > 9 ? 2 : 1), p->total_ticks);
    printf("| MLFQ Level: Q%-16d |\n", p->queue_level);
    printf("+------------------------------+\n\n");
}

/* ================================================================
 * list_pcb — list all processes in table format
 * ================================================================ */

void pcb_list(SystemState *s) {
    if (s->process_count == 0) {
        printf("(No active processes)\n");
        return;
    }

    printf("\n%4s %-16s %-10s %3s %6s %6s  %s\n",
           "PID", "NAME", "STATE", "PRI", "MEM", "TICKS", "OWNER");
    printf("---- ---------------- ---------- --- ------ ------  --------\n");

    PCB *p = s->process_list;
    while (p) {
        printf("%4d %-16s %-10s %3d %5dK %5d   %s",
               p->pid,
               p->name,
               pcb_state_string(p->state),
               p->priority,
               p->mem_size,
               p->total_ticks,
               p->owner);

        if (s->running == p) printf("  <-- running");
        printf("\n");
        p = p->next;
    }
    printf("---- ---------------- ---------- --- ------ ------  --------\n");
    printf("Total: %d process(es)\n\n", s->process_count);
}

/* ================================================================
 * ptree — tree display of process hierarchy
 * ================================================================ */

static void ptree_print(PCB *p, int depth, const char *prefix) {
    if (!p) return;

    char mark[4] = "   ";
    if (p->state == STATE_RUNNING)   strcpy(mark, "RUN");
    else if (p->state == STATE_READY)   strcpy(mark, "RDY");
    else if (p->state == STATE_BLOCKED) strcpy(mark, "BLK");
    else if (p->state == STATE_SUSPENDED) strcpy(mark, "SUS");

    printf("%s%s%s(%d) [%s, prio=%d, mem=%dKB]\n",
           prefix,
           (depth > 0) ? "+- " : "",
           p->name, p->pid,
           mark,
           p->priority,
           p->mem_size);

    char new_prefix[512];
    snprintf(new_prefix, sizeof(new_prefix), "%s%s",
             prefix, (depth > 0) ? "|  " : "   ");

    PCB *child = p->first_child;
    while (child) {
        ptree_print(child, depth + 1, new_prefix);
        child = child->next_sibling;
    }
}

void pcb_ptree(SystemState *s) {
    if (s->process_count == 0) {
        printf("(Process tree is empty)\n");
        return;
    }

    printf("\n=== Process Tree ===\n");

    PCB *root = pcb_find(s, 1);
    if (root) {
        ptree_print(root, 0, "");
    }

    PCB *p = s->process_list;
    int orphans = 0;
    while (p) {
        if (p->pid != 1 && !p->parent && !pcb_find(s, p->ppid)) {
            if (orphans == 0) printf("  [orphan processes]\n");
            ptree_print(p, 1, "");
            orphans++;
        }
        p = p->next;
    }
    printf("\n");
}
