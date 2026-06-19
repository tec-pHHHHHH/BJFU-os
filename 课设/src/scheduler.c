/**
 * scheduler.c — MLFQ scheduler implementation
 *
 * Algorithm (PPT slide 5):
 *   Higher-priority queue first; round-robin within queue; time slice exhausted -> demotion
 *   Q0(p0-3, q=2) -> Q1(p4-7, q=4) -> Q2(p8-15, q=8)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "include/os_types.h"
#include "include/process.h"
#include "include/scheduler.h"

#define DEFAULT_SPEED_MS  1000

/* ================================================================
 * Print single queue
 * ================================================================ */

static void print_one_queue(MLFQ_Level *q, int level) {
    printf("Q%d(prio %d-%d, quantum=%d): ",
           level, q->prio_min, q->prio_max, q->quantum);

    if (q->count == 0) {
        printf("(empty)\n");
        return;
    }

    PCB *p = q->head;
    int first = 1;
    if (p) {
        do {
            if (!first) printf(" -> ");
            printf("%s(PID=%d,rem=%d)",
                   p->name, p->pid, p->time_slice);
            first = 0;
            p = p->queue_next;
        } while (p != q->head);
    }
    printf("\n");
}

/* ================================================================
 * Print MLFQ snapshot
 * ================================================================ */

void scheduler_show_queues(SystemState *s) {
    printf("\n-- MLFQ Snapshot --\n");
    for (int i = 0; i < MLFQ_LEVELS; i++) {
        print_one_queue(&s->scheduler.levels[i], i);
    }
    printf("-- Clock: %d ticks --\n\n", s->system_clock);
}

/* ================================================================
 * Core: execute one scheduling tick
 * ================================================================ */

void scheduler_tick(SystemState *s) {
    int level = -1;
    PCB *selected = NULL;

    for (int i = 0; i < MLFQ_LEVELS; i++) {
        if (s->scheduler.levels[i].count > 0) {
            level = i;
            selected = s->scheduler.levels[i].head;
            break;
        }
    }

    if (!selected) {
        printf("[Sched] All queues empty — no process to schedule.\n");
        return;
    }

    pcb_dequeue(s, selected);

    selected->state = STATE_RUNNING;
    s->running = selected;

    int quantum = s->scheduler.levels[level].quantum;
    int run_ticks = (selected->time_slice < quantum)
                    ? selected->time_slice : quantum;

    s->system_clock += run_ticks;
    selected->total_ticks   += run_ticks;
    selected->time_slice    -= run_ticks;

    printf("\n=====================================\n");
    printf("[Sched tick=%d] Selected: %s(PID=%d) from Q%d\n",
           s->system_clock, selected->name, selected->pid, level);
    printf("  priority=%d, executed %d ticks, timeslice remaining=%d\n",
           selected->priority, run_ticks, selected->time_slice);

    if (selected->time_slice <= 0) {
        int new_level = level;
        if (level < MLFQ_LEVELS - 1) {
            new_level = level + 1;
        }
        selected->time_slice = s->scheduler.levels[new_level].quantum;

        printf("  >>> Timeslice exhausted! Demotion: Q%d -> Q%d (new timeslice=%d)\n",
               level, new_level, selected->time_slice);

        selected->queue_level = new_level;
        MLFQ_Level *q = &s->scheduler.levels[new_level];
        selected->queue_next = NULL;
        selected->queue_prev = NULL;

        if (!q->head) {
            q->head = q->tail = selected;
            selected->queue_next = selected->queue_prev = selected;
        } else {
            selected->queue_prev = q->tail;
            selected->queue_next = q->head;
            q->tail->queue_next = selected;
            q->head->queue_prev = selected;
            q->tail = selected;
        }
        q->count++;
    } else {
        printf("  OK  Timeslice not exhausted, back to Q%d tail\n", level);
        pcb_enqueue_ready(s, selected);
    }

    selected->state = STATE_READY;
    s->running = NULL;
    s->dirty = 1;
    printf("=====================================\n");

    scheduler_show_queues(s);
}

/* ================================================================
 * Scheduler thread — auto mode
 * ================================================================ */

void *scheduler_thread_func(void *arg) {
    SystemState *s = (SystemState *)arg;

    printf("[Scheduler] Auto-scheduler thread started (speed=%dms/tick).\n", s->sched_speed);

    while (s->sched_running) {
        pthread_mutex_lock(&s->sched_mutex);
        while (s->sched_paused && s->sched_running) {
            printf("[Scheduler] Paused, waiting to resume...\n");
            pthread_cond_wait(&s->sched_cond, &s->sched_mutex);
        }
        pthread_mutex_unlock(&s->sched_mutex);

        if (!s->sched_running) break;

        scheduler_tick(s);

        if (s->sched_speed > 0) {
            usleep(s->sched_speed * 1000);
        }
    }

    printf("[Scheduler] Scheduler thread exited.\n");
    return NULL;
}

/* ================================================================
 * start_sched — start auto scheduling
 * ================================================================ */

int scheduler_start(SystemState *s) {
    if (s->sched_running && !s->sched_paused) {
        printf("[Hint] Scheduler is already running.\n");
        return 0;
    }

    if (s->sched_running && s->sched_paused) {
        return scheduler_restart(s);
    }

    int total = 0;
    for (int i = 0; i < MLFQ_LEVELS; i++)
        total += s->scheduler.levels[i].count;
    if (total == 0) {
        printf("[Hint] No schedulable processes, create one first (create_pcb).\n");
        return -1;
    }

    pthread_mutex_init(&s->sched_mutex, NULL);
    pthread_cond_init(&s->sched_cond, NULL);
    s->sched_speed  = DEFAULT_SPEED_MS;
    s->sched_running = 1;
    s->sched_paused  = 0;

    if (pthread_create(&s->sched_tid, NULL, scheduler_thread_func, s) != 0) {
        perror("pthread_create (scheduler)");
        s->sched_running = 0;
        return -1;
    }

    printf("[Scheduler] start_sched — auto scheduler started.\n");
    return 0;
}

/* ================================================================
 * stop_sched — pause auto scheduling, preserve state
 * ================================================================ */

int scheduler_stop(SystemState *s) {
    if (!s->sched_running) {
        printf("[Hint] Scheduler is not running.\n");
        return 0;
    }
    if (s->sched_paused) {
        printf("[Hint] Scheduler is already paused.\n");
        return 0;
    }

    pthread_mutex_lock(&s->sched_mutex);
    s->sched_paused = 1;
    pthread_mutex_unlock(&s->sched_mutex);

    printf("[Scheduler] stop_sched — Paused. Current state:\n");
    scheduler_show_queues(s);
    printf("  Hint: use restart_sched to resume, step for single-step.\n");
    return 0;
}

/* ================================================================
 * restart_sched — resume auto scheduling
 * ================================================================ */

int scheduler_restart(SystemState *s) {
    if (!s->sched_running) {
        printf("[Hint] Scheduler not started, use start_sched first.\n");
        return -1;
    }
    if (!s->sched_paused) {
        printf("[Hint] Scheduler is not paused, no need to restart.\n");
        return 0;
    }

    pthread_mutex_lock(&s->sched_mutex);
    s->sched_paused = 0;
    pthread_cond_signal(&s->sched_cond);
    pthread_mutex_unlock(&s->sched_mutex);

    printf("[Scheduler] restart_sched — Auto scheduler resumed.\n");
    return 0;
}

/* ================================================================
 * step — single-step execution with full decision chain output
 * ================================================================ */

int scheduler_step(SystemState *s) {
    int total = 0;
    for (int i = 0; i < MLFQ_LEVELS; i++)
        total += s->scheduler.levels[i].count;
    if (total == 0) {
        printf("[Step] No schedulable processes.\n");
        return -1;
    }

    printf("\n+==================================+\n");
    printf("|       STEP Single-Step           |\n");
    printf("+==================================+\n");

    /* Step 1: current queue snapshot */
    printf("\n[Step 1] Current queue snapshot:\n");
    scheduler_show_queues(s);

    /* Step 2: selection decision */
    printf("[Step 2] Selection logic:\n");
    int level = -1;
    for (int i = 0; i < MLFQ_LEVELS; i++) {
        if (s->scheduler.levels[i].count > 0) {
            level = i;
            printf("  -> Scan Q%d: non-empty (%d processes)\n",
                   i, s->scheduler.levels[i].count);
            break;
        } else {
            printf("  -> Scan Q%d: empty, skip\n", i);
        }
    }
    printf("  -> Selected Q%d head: %s(PID=%d, prio=%d, remaining timeslice=%d)\n\n",
           level,
           s->scheduler.levels[level].head->name,
           s->scheduler.levels[level].head->pid,
           s->scheduler.levels[level].head->priority,
           s->scheduler.levels[level].head->time_slice);

    /* Step 3: execute */
    printf("[Step 3] Execution:\n");
    int clock_before = s->system_clock;
    scheduler_tick(s);
    int ticks_exec = s->system_clock - clock_before;
    printf("  -> Executed %d ticks, system clock now %d\n\n",
           ticks_exec, s->system_clock);

    /* Step 4: final state */
    printf("[Step 4] Final state:\n");
    scheduler_show_queues(s);

    return 0;
}
