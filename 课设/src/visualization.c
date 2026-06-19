/**
 * visualization.c — Global Overview Implementation
 *
 * overview command: unified view of Process Tree + Memory Map + MLFQ Queues
 * PPT acceptance: all 3 panels required; must reflect changes after operations
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "include/os_types.h"
#include "include/process.h"
#include "include/memory.h"
#include "include/scheduler.h"
#include "include/visualization.h"

/* ================================================================
 * Internal: recursive process tree printer
 * ================================================================ */

static void tree_print(PCB *p, int depth, const char *prefix) {
    if (!p) return;

    char mark[5] = "    ";
    if      (p->state == STATE_RUNNING)   snprintf(mark, sizeof(mark), "RUN ");
    else if (p->state == STATE_READY)     snprintf(mark, sizeof(mark), "RDY ");
    else if (p->state == STATE_BLOCKED)   snprintf(mark, sizeof(mark), "BLK ");
    else if (p->state == STATE_SUSPENDED) snprintf(mark, sizeof(mark), "SUS ");

    /* memory usage bar */
    char membar[16];
    int bar_len = (p->mem_size > 0) ? (p->mem_size * 12 / 1024) + 1 : 1;
    if (bar_len > 12) bar_len = 12;
    memset(membar, '=', bar_len);
    membar[bar_len] = '\0';

    printf("%s%s%-12s PID=%-3d [%s] prio=%-2d mem=%3dKB %s\n",
           prefix,
           (depth > 0) ? "+- " : "",
           p->name, p->pid,
           mark,
           p->priority,
           p->mem_size,
           (p->mem_size > 0) ? membar : "");

    char new_prefix[512];
    snprintf(new_prefix, sizeof(new_prefix), "%s%s",
             prefix, (depth > 0) ? "|  " : "   ");

    PCB *child = p->first_child;
    while (child) {
        tree_print(child, depth + 1, new_prefix);
        child = child->next_sibling;
    }
}

/* ================================================================
 * Internal: memory ASCII map
 * ================================================================ */

static void memory_map_print(SystemState *s) {
    typedef struct { int start; int size; int type; int pid; } Chunk;
    Chunk chunks[MAX_PROCESSES * 2];
    int nc = 0;

    /* collect free blocks */
    FreeBlock *fb = s->free_list;
    while (fb && nc < MAX_PROCESSES * 2) {
        chunks[nc].start = fb->start;
        chunks[nc].size  = fb->size;
        chunks[nc].type  = 0;
        chunks[nc].pid   = 0;
        nc++;
        fb = fb->next;
    }

    /* collect allocated blocks */
    AllocBlock *ab = s->alloc_list;
    while (ab && nc < MAX_PROCESSES * 2) {
        chunks[nc].start = ab->start;
        chunks[nc].size  = ab->size;
        chunks[nc].type  = 1;
        chunks[nc].pid   = ab->pid;
        nc++;
        ab = ab->next;
    }

    /* sort by address */
    for (int i = 0; i < nc - 1; i++)
        for (int j = 0; j < nc - 1 - i; j++)
            if (chunks[j].start > chunks[j+1].start) {
                Chunk tmp = chunks[j];
                chunks[j] = chunks[j+1];
                chunks[j+1] = tmp;
            }

    /* compute stats */
    int total_free = 0, total_used = 0, max_free = 0;
    for (int i = 0; i < nc; i++) {
        if (chunks[i].type == 0) {
            total_free += chunks[i].size;
            if (chunks[i].size > max_free) max_free = chunks[i].size;
        } else {
            total_used += chunks[i].size;
        }
    }

    /* scale ruler */
    printf("     0");
    for (int i = 1; i <= 10; i++) {
        for (int s = 0; s < 9; s++) putchar(' ');
        printf("%-4d", TOTAL_MEMORY * i / 10);
    }
    printf(" KB\n");

    /* ASCII bar chart (~60 chars wide) */
    printf("     +");
    for (int i = 0; i < 60; i++) putchar('-');
    printf("+\n     |");

    for (int i = 0; i < nc; i++) {
        int w = chunks[i].size * 60 / TOTAL_MEMORY;
        if (w < 1) w = 1;

        if (chunks[i].type == 1) {
            PCB *p = pcb_find(s, chunks[i].pid);
            char label = p ? p->name[0] : 'K';
            for (int j = 0; j < w; j++) putchar(label);
        } else {
            for (int j = 0; j < w; j++) putchar('.');
        }
    }
    printf("|\n");

    /* legend + block details */
    printf("     +");
    for (int i = 0; i < 60; i++) putchar('-');
    printf("+\n\n");
    printf("     Legend: '.' = free  |  letter = allocated (first char of name)\n\n");

    int cursor = 0;
    for (int i = 0; i < nc; i++) {
        if (chunks[i].start > cursor) {
            printf("     [GAP]  %4d KB ~ %4d KB  (%d KB unmanaged)\n",
                   cursor, chunks[i].start - 1,
                   chunks[i].start - cursor);
        }
        if (chunks[i].type == 1) {
            PCB *p = pcb_find(s, chunks[i].pid);
            printf("     [USED] %4d KB ~ %4d KB  (%3d KB)  %s(PID=%d)\n",
                   chunks[i].start,
                   chunks[i].start + chunks[i].size - 1,
                   chunks[i].size,
                   p ? p->name : "kernel",
                   chunks[i].pid);
        } else {
            printf("     [FREE] %4d KB ~ %4d KB  (%3d KB)\n",
                   chunks[i].start,
                   chunks[i].start + chunks[i].size - 1,
                   chunks[i].size);
        }
        cursor = chunks[i].start + chunks[i].size;
    }
    if (cursor < TOTAL_MEMORY) {
        printf("     [FREE] %4d KB ~ %4d KB  (%3d KB)\n",
               cursor, TOTAL_MEMORY - 1, TOTAL_MEMORY - cursor);
    }

    /* fragmentation rate */
    float frag = 0.0f;
    if (total_free > 0) {
        frag = 1.0f - (float)max_free / (float)total_free;
    }
    printf("\n     Total: %d KB | Used: %d KB | Free: %d KB | Frag: %.1f%% | Algo: %s\n",
           TOTAL_MEMORY, total_used, total_free, frag * 100.0f,
           s->alloc_algo == ALLOC_FIRST_FIT  ? "First-Fit" :
           s->alloc_algo == ALLOC_BEST_FIT   ? "Best-Fit" : "Worst-Fit");
}

/* ================================================================
 * Internal: MLFQ queue status printer
 * ================================================================ */

static void mlfq_print(SystemState *s) {
    for (int i = 0; i < MLFQ_LEVELS; i++) {
        MLFQ_Level *q = &s->scheduler.levels[i];
        printf("     Q%d (prio %2d~%2d | quantum=%d) [%2d procs] ",
               i, q->prio_min, q->prio_max, q->quantum, q->count);

        if (q->count == 0 || !q->head) {
            printf("-- empty queue\n");
            continue;
        }

        PCB *cur = q->head;
        int first = 1;
        do {
            if (!first) printf(" -> ");
            char st;
            switch (cur->state) {
                case STATE_RUNNING:   st = 'R'; break;
                case STATE_READY:     st = 'Y'; break;
                case STATE_BLOCKED:   st = 'B'; break;
                case STATE_SUSPENDED: st = 'S'; break;
                default:              st = '?'; break;
            }
            printf("%s(PID=%d,%c,rem=%d)",
                   cur->name, cur->pid, st, cur->time_slice);
            first = 0;
            cur = cur->queue_next;
        } while (cur != q->head);
        printf("\n");
    }
}

/* ================================================================
 * overview — global visualization entry point
 * ================================================================ */

void overview(SystemState *s) {
    printf("\n");
    printf("  ======================================================================\n");
    printf("  |             OS Core Simulator — Global Overview                    |\n");
    printf("  |====================================================================|\n");
    printf("  |  Clock: %-6d ticks | Procs: %-3d | User: %-16s    |\n",
           s->system_clock, s->process_count,
           s->current_user[0] ? s->current_user : "guest");
    printf("  |--------------------------------------------------------------------|\n");
    printf("  |  Scheduler: %-8s | Autosave: %-4s | PID pool: next=%-4d     |\n",
           s->sched_running ? (s->sched_paused ? "PAUSED" : "RUNNING") : "STOPPED",
           s->autosave ? "ON" : "OFF",
           s->next_pid);
    printf("  ======================================================================\n");

    /* --- Panel 1: Process Tree --- */
    printf("\n  +--- Panel 1: Process Tree ------------------------------------------+\n");
    if (s->process_count == 0) {
        printf("  |  (empty)                                                          |\n");
    } else {
        PCB *root = pcb_find(s, 1);
        if (root) {
            tree_print(root, 0, "  |  ");
        }
        /* orphan processes */
        PCB *p = s->process_list;
        while (p) {
            if (p->pid != 1 && !p->parent && !pcb_find(s, p->ppid)) {
                printf("  |  [orphan] ");
                tree_print(p, 1, "  |  ");
            }
            p = p->next;
        }
    }
    printf("  +--------------------------------------------------------------------+\n");

    /* --- Panel 2: Memory Layout --- */
    printf("\n  +--- Panel 2: Memory ASCII Map --------------------------------------+\n");
    printf("  |                                                                    |\n");
    memory_map_print(s);
    printf("  |                                                                    |\n");
    printf("  +--------------------------------------------------------------------+\n");

    /* --- Panel 3: MLFQ Queues --- */
    printf("\n  +--- Panel 3: MLFQ Ready Queues -------------------------------------+\n");
    mlfq_print(s);
    printf("  +--------------------------------------------------------------------+\n\n");
}
