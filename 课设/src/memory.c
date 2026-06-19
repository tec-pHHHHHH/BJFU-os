/**
 * memory.c — Dynamic partition memory management
 *
 *   Allocation algorithms: First-Fit(FF) / Best-Fit(BF) / Worst-Fit(WF)
 *   Defragmentation: compact — relocation-based compaction
 *   Fragmentation rate = 1 - max_contiguous_free / total_free
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "include/os_types.h"
#include "include/process.h"
#include "include/memory.h"

/* ================================================================
 * Internal: insert free block sorted by address, auto-merge adjacent
 * ================================================================ */
//按内存地址从小到大，把新空闲块fb插入空闲链表 + 自动合并相邻空闲块
static void free_list_insert_sorted(SystemState *s, FreeBlock *fb) {
    FreeBlock **pp = &s->free_list;
    while (*pp && (*pp)->start < fb->start) {
        pp = &(*pp)->next;
    }
    fb->next = *pp;
    *pp = fb;

    FreeBlock *curr = s->free_list;
    while (curr && curr->next) {
        if (curr->start + curr->size == curr->next->start) {
            FreeBlock *merged = curr->next;
            curr->size += merged->size;
            curr->next = merged->next;
            free(merged);
        } else {
            curr = curr->next;
        }
    }
}

/* ================================================================
 * Find suitable free block using current algorithm
 * ================================================================ */

FreeBlock* mem_find_free(SystemState *s, int size) {
    FreeBlock *fb   = s->free_list;
    FreeBlock *best = NULL;

    switch (s->alloc_algo) {

        case ALLOC_FIRST_FIT:
            while (fb) {
                if (fb->size >= size) return fb;
                fb = fb->next;
            }
            return NULL;

        case ALLOC_BEST_FIT:
            while (fb) {
                if (fb->size >= size &&
                    (!best || fb->size < best->size)) {
                    best = fb;
                }
                fb = fb->next;
            }
            return best;

        case ALLOC_WORST_FIT:
            while (fb) {
                if (fb->size >= size &&
                    (!best || fb->size > best->size)) {
                    best = fb;
                }
                fb = fb->next;
            }
            return best;

        default:
            return NULL;
    }
}

/* ================================================================
 * alloc <size> — allocate memory
 * ================================================================ */

int mem_alloc(SystemState *s, int size) {
    if (size < MIN_ALLOC) {
        printf("[Error] Minimum allocation unit is %d KB.\n", MIN_ALLOC);
        return -1;
    }

    FreeBlock *fb = mem_find_free(s, size);
    if (!fb) {
        printf("[Error] Out of memory! Requested %d KB, no suitable free block.\n", size);
        return -1;
    }

    int alloc_start = fb->start;

    AllocBlock *ab = malloc(sizeof(AllocBlock));
    ab->start = fb->start;
    ab->size  = size;
    ab->pid   = (s->running) ? s->running->pid : 0;
    ab->next  = s->alloc_list;
    s->alloc_list = ab;

    if (fb->size == size) {
        FreeBlock **pp = &s->free_list;
        while (*pp && *pp != fb) pp = &(*pp)->next;
        if (*pp) *pp = fb->next;
        free(fb);
    } else {
        fb->start += size;
        fb->size  -= size;
    }

    int pid = ab->pid;
    if (pid > 0) {
        PCB *p = pcb_find(s, pid);
        if (p) {
            p->mem_start = alloc_start;
            p->mem_size  = size;
        }
    }

    s->dirty = 1;
    printf("[Alloc] Success! addr=%d KB, size=%d KB", alloc_start, size);
    if (pid > 0) printf(", bound to PID=%d", pid);
    printf(" (algo: %s)\n",
           s->alloc_algo == ALLOC_FIRST_FIT  ? "First-Fit" :
           s->alloc_algo == ALLOC_BEST_FIT   ? "Best-Fit" : "Worst-Fit");
    return alloc_start;
}

/* ================================================================
 * free_mem <addr> — free memory
 * ================================================================ */

int mem_free(SystemState *s, int start_addr) {
    AllocBlock *prev = NULL, *ab = s->alloc_list;
    while (ab) {
        if (ab->start == start_addr) break;
        prev = ab;
        ab = ab->next;
    }

    if (!ab) {
        printf("[Error] No allocated memory at address %d KB.\n", start_addr);
        return -1;
    }

    int freed_size  = ab->size;
    int freed_pid   = ab->pid;

    if (prev) prev->next = ab->next;
    else      s->alloc_list = ab->next;
    free(ab);

    FreeBlock *fb = malloc(sizeof(FreeBlock));
    fb->start = start_addr;
    fb->size  = freed_size;
    free_list_insert_sorted(s, fb);

    if (freed_pid > 0) {
        PCB *p = pcb_find(s, freed_pid);
        if (p) {
            p->mem_start = -1;
            p->mem_size  = 0;
        }
    }

    s->dirty = 1;
    printf("[Free] addr=%d KB, size=%d KB returned to free list.\n",
           start_addr, freed_size);
    return 0;
}

/* ================================================================
 * compact — relocation-based defragmentation（基于重定位的碎片整理）
 * ================================================================ */

void mem_compact(SystemState *s) {
    if (!s->alloc_list) {
        printf("[Compact] No allocated blocks, nothing to compact.\n");
        return;
    }

    int before_free_blocks = 0;
    FreeBlock *fb = s->free_list;
    while (fb) { before_free_blocks++; fb = fb->next; }

    int count = 0;
    AllocBlock **arr = malloc(sizeof(AllocBlock*) * MAX_PROCESSES);
    AllocBlock *ab = s->alloc_list;
    while (ab && count < MAX_PROCESSES) {
        arr[count++] = ab;
        ab = ab->next;
    }

    for (int i = 0; i < count - 1; i++)
        for (int j = 0; j < count - 1 - i; j++)
            if (arr[j]->start > arr[j+1]->start) {
                AllocBlock *tmp = arr[j];
                arr[j] = arr[j+1];
                arr[j+1] = tmp;
            }

    int cursor = 0;//作为下一个块的新起始地址
    for (int i = 0; i < count; i++) {
        int old_start = arr[i]->start;
        int moved = (old_start != cursor);
        arr[i]->start = cursor;
        cursor += arr[i]->size;

        if (arr[i]->pid > 0) {
            PCB *p = pcb_find(s, arr[i]->pid);
            if (p) p->mem_start = arr[i]->start;
        }
        if (moved) {
            printf("  [Relocate] block(PID=%d): %d KB -> %d KB\n",
                   arr[i]->pid, old_start, arr[i]->start);
        }
    }

    FreeBlock *old = s->free_list;
    while (old) {
        FreeBlock *next = old->next;
        free(old);
        old = next;
    }

    if (cursor < TOTAL_MEMORY) {
        s->free_list = malloc(sizeof(FreeBlock));
        s->free_list->start = cursor;
        s->free_list->size  = TOTAL_MEMORY - cursor;
        s->free_list->next  = NULL;
    } else {
        s->free_list = NULL;
    }

    free(arr);
    s->dirty = 1;

    printf("[Compact] Done! All allocated blocks moved to low addresses.\n");
    printf("  Free blocks: %d -> %d\n",
           before_free_blocks, s->free_list ? 1 : 0);
    mem_show(s);
}

/* ================================================================
 * show_mem — visualize memory layout
 * ================================================================ */

void mem_show(SystemState *s) {
    printf("\n====== Memory Layout (0-%d KB) ======\n", TOTAL_MEMORY);

    printf("[ASCII]:\n");
    printf("|");
    int cursor = 0;
    FreeBlock *fb = s->free_list;
    AllocBlock *ab = s->alloc_list;

    typedef struct { int start; int size; int type; int pid; } Chunk;
    Chunk chunks[MAX_PROCESSES * 2];
    int nc = 0;

    fb = s->free_list;
    while (fb && nc < MAX_PROCESSES * 2) {
        chunks[nc].start = fb->start;
        chunks[nc].size  = fb->size;
        chunks[nc].type  = 0;
        chunks[nc].pid   = 0;
        nc++;
        fb = fb->next;
    }

    ab = s->alloc_list;
    while (ab && nc < MAX_PROCESSES * 2) {
        chunks[nc].start = ab->start;
        chunks[nc].size  = ab->size;
        chunks[nc].type  = 1;
        chunks[nc].pid   = ab->pid;
        nc++;
        ab = ab->next;
    }

    for (int i = 0; i < nc - 1; i++)
        for (int j = 0; j < nc - 1 - i; j++)
            if (chunks[j].start > chunks[j+1].start) {
                Chunk tmp = chunks[j];
                chunks[j] = chunks[j+1];
                chunks[j+1] = tmp;
            }

    for (int i = 0; i < nc; i++) {
        if (chunks[i].start > cursor) {
            printf("--gap(%d)--", chunks[i].start - cursor);
        }
        if (chunks[i].type == 1) {
            PCB *p = pcb_find(s, chunks[i].pid);
            printf("##%s(%d)##",
                   p ? p->name : "kernel",
                   chunks[i].size);
        } else {
            printf("--free(%d)--", chunks[i].size);
        }
        cursor = chunks[i].start + chunks[i].size;
    }
    if (cursor < TOTAL_MEMORY) {
        printf("--free(%d)--", TOTAL_MEMORY - cursor);
    }
    printf("|\n\n");

    printf("[Free Block List] (algorithm: %s):\n",
           s->alloc_algo == ALLOC_FIRST_FIT  ? "First-Fit" :
           s->alloc_algo == ALLOC_BEST_FIT   ? "Best-Fit" : "Worst-Fit");
    if (!s->free_list) {
        printf("  (no free blocks)\n");
    } else {
        fb = s->free_list;
        while (fb) {
            printf("  [FREE] start=%4d KB, size=%4d KB\n", fb->start, fb->size);
            fb = fb->next;
        }
    }

    printf("[Allocated Block List]:\n");
    if (!s->alloc_list) {
        printf("  (no allocated blocks)\n");
    } else {
        ab = s->alloc_list;
        while (ab) {
            PCB *p = pcb_find(s, ab->pid);
            printf("  [USED] start=%4d KB, size=%4d KB, PID=%d (%s)\n",
                   ab->start, ab->size, ab->pid,
                   p ? p->name : "kernel");
            ab = ab->next;
        }
    }
    printf("\n");
}

/* ================================================================
 * mem_stat — memory statistics
 * ================================================================ */

void mem_stat(SystemState *s) {
    int total_free   = 0;
    int total_alloc  = 0;
    int max_free     = 0;
    int free_blocks  = 0;
    int alloc_blocks = 0;

    FreeBlock *fb = s->free_list;
    while (fb) {
        total_free += fb->size;
        if (fb->size > max_free) max_free = fb->size;
        free_blocks++;
        fb = fb->next;
    }

    AllocBlock *ab = s->alloc_list;
    while (ab) {
        total_alloc += ab->size;
        alloc_blocks++;
        ab = ab->next;
    }

    float frag_rate = 0.0f;
    if (total_free > 0) {
        frag_rate = 1.0f - (float)max_free / (float)total_free;
    }

    printf("\n====== Memory Statistics ======\n");
    printf("  Total:        %4d KB\n", TOTAL_MEMORY);
    printf("  Used:         %4d KB (%d block(s))\n", total_alloc, alloc_blocks);
    printf("  Free:         %4d KB (%d block(s))\n", total_free, free_blocks);
    printf("  Max contiguous free: %4d KB\n", max_free);
    printf("  Fragmentation: %.1f%%\n", frag_rate * 100.0f);

    if (frag_rate > 0.05f && free_blocks > 1) {
        printf("  WARNING: External fragmentation detected! Consider 'compact'.\n");
    } else if (free_blocks <= 1) {
        printf("  OK: Free space is contiguous, no external fragmentation.\n");
    }
    printf("\n");
}

/* ================================================================
 * set_alloc_algo <n> — switch allocation algorithm
 * ================================================================ */

void mem_set_algo(SystemState *s, int algo) {
    if (algo < 0 || algo > 2) {
        printf("[Error] Algo: 0=First-Fit, 1=Best-Fit, 2=Worst-Fit.\n");
        return;
    }
    s->alloc_algo = (AllocAlgorithm)algo;
    const char *names[] = {"First-Fit (FF)", "Best-Fit (BF)", "Worst-Fit (WF)"};
    printf("[Algo] Switched to: %s.\n", names[algo]);
    s->dirty = 1;
}

/* ================================================================
 * pgfault — simulate page fault
 * ================================================================ */

void mem_pgfault(SystemState *s) {
    (void)s;
    printf("\n+====================================+\n");
    printf("|  !! PAGE FAULT                     |\n");
    printf("|  Page not present in physical mem  |\n");
    printf("|  Page fault handler invoked...     |\n");
    printf("|  Swapping page in from disk        |\n");
    printf("|  Page table updated                |\n");
    printf("|  Fault handled, process resumes    |\n");
    printf("+====================================+\n\n");
}

/* ================================================================
 * swap_out <pid> — swap process memory out to disk
 * ================================================================ */

int mem_swap_out(SystemState *s, int pid) {
    PCB *p = pcb_find(s, pid);
    if (!p) {
        printf("[Error] Process PID=%d not found.\n", pid);
        return -1;
    }

    if (p->mem_size <= 0 || p->mem_start < 0) {
        printf("[Hint] Process %s (PID=%d) has no allocated memory.\n",
               p->name, pid);
        return 0;
    }

    int old_start = p->mem_start;
    int old_size  = p->mem_size;

    AllocBlock *prev = NULL, *ab = s->alloc_list;
    while (ab) {
        if (ab->pid == pid && ab->start == old_start) break;
        prev = ab;
        ab = ab->next;
    }

    if (ab) {
        if (prev) prev->next = ab->next;
        else      s->alloc_list = ab->next;
        free(ab);
    }

    FreeBlock *fb = malloc(sizeof(FreeBlock));
    fb->start = old_start;
    fb->size  = old_size;
    free_list_insert_sorted(s, fb);

    p->mem_start = -1;
    p->mem_size  = 0;

    s->dirty = 1;
    printf("[SwapOut] Process %s(PID=%d): %d KB (addr %d) -> swap space.\n",
           p->name, pid, old_size, old_start);
    printf("  (Simulated: memory freed, data marked as in swap)\n");
    return 0;
}
