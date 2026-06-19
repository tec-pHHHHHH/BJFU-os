/**
 * persistence.c — State persistence implementation
 *
 * Binary file format (STATE_FILE = "data/os_state.bin"):
 *
 *   [Header: 9 ints]
 *     magic | version | pcb_count | next_pid | system_clock
 *     | sched_running | alloc_algo | free_block_count | alloc_block_count
 *
 *   [PCB Records] x pcb_count
 *     Each: pid, ppid, name[32], owner[32], state, priority,
 *           mem_start, mem_size, time_slice, total_ticks, queue_level
 *
 *   [MLFQ: 3 levels]
 *     Each: count(int) + pid[count](int...)
 *
 *   [Free Blocks] x free_block_count
 *     Each: start(int), size(int)
 *
 *   [Alloc Blocks] x alloc_block_count
 *     Each: start(int), size(int), pid(int)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "include/os_types.h"
#include "include/process.h"
#include "include/persistence.h"

#define PERSIST_VERSION  1

/* PCB serialization record (fixed-size for block read/write) */
typedef struct {
    int  pid, ppid;
    char name[MAX_NAME_LEN];
    char owner[MAX_NAME_LEN];
    int  state, priority;
    int  mem_start, mem_size;
    int  time_slice, total_ticks, queue_level;
} PCBRecord;

/* ================================================================
 * Helper: pack PCB into record
 * ================================================================ */

static void pcb_to_record(PCB *p, PCBRecord *r) {
    memset(r, 0, sizeof(*r));
    r->pid       = p->pid;
    r->ppid      = p->ppid;
    strncpy(r->name,  p->name,  MAX_NAME_LEN - 1);
    strncpy(r->owner, p->owner, MAX_NAME_LEN - 1);
    r->state       = (int)p->state;
    r->priority    = p->priority;
    r->mem_start   = p->mem_start;
    r->mem_size    = p->mem_size;
    r->time_slice  = p->time_slice;
    r->total_ticks = p->total_ticks;
    r->queue_level = p->queue_level;
}

/* ================================================================
 * Helper: restore PCB from record
 * ================================================================ */

static PCB* record_to_pcb(PCBRecord *r) {
    PCB *p = calloc(1, sizeof(PCB));
    if (!p) return NULL;
    p->pid         = r->pid;
    p->ppid        = r->ppid;
    strncpy(p->name,  r->name,  MAX_NAME_LEN - 1);
    strncpy(p->owner, r->owner, MAX_NAME_LEN - 1);
    p->state       = (ProcessState)r->state;
    p->priority    = r->priority;
    p->mem_start   = r->mem_start;
    p->mem_size    = r->mem_size;
    p->time_slice  = r->time_slice;
    p->total_ticks = r->total_ticks;
    p->queue_level = r->queue_level;
    p->parent      = NULL;
    p->first_child = NULL;
    p->next_sibling = NULL;
    p->next        = NULL;
    p->queue_next  = NULL;
    p->queue_prev  = NULL;
    return p;
}

/* ================================================================
 * Clear current system state (called before load)
 * ================================================================ */

static void clear_state(SystemState *s) {
    PCB *p = s->process_list;
    while (p) {
        PCB *next = p->next;
        free(p);
        p = next;
    }
    s->process_list  = NULL;
    s->process_count = 0;
    s->running       = NULL;

    FreeBlock *fb = s->free_list;
    while (fb) {
        FreeBlock *next = fb->next;
        free(fb);
        fb = next;
    }
    s->free_list = NULL;

    AllocBlock *ab = s->alloc_list;
    while (ab) {
        AllocBlock *next = ab->next;
        free(ab);
        ab = next;
    }
    s->alloc_list = NULL;

    for (int i = 0; i < MLFQ_LEVELS; i++) {
        s->scheduler.levels[i].head  = NULL;
        s->scheduler.levels[i].tail  = NULL;
        s->scheduler.levels[i].count = 0;
    }
}

/* ================================================================
 * save — full state snapshot
 * ================================================================ */

int persistence_save(SystemState *s) {
    FILE *fp = fopen(STATE_FILE, "wb");
    if (!fp) {
        perror("persistence_save: fopen");
        return -1;
    }

    int pcb_count = s->process_count;

    int free_count = 0;
    FreeBlock *fb = s->free_list;
    while (fb) { free_count++; fb = fb->next; }

    int alloc_count = 0;
    AllocBlock *ab = s->alloc_list;
    while (ab) { alloc_count++; ab = ab->next; }

    /* ---- Write Header ---- */
    int magic          = OS_MAGIC;
    int version        = PERSIST_VERSION;
    int clock          = s->system_clock;
    int sched_run      = s->sched_running;
    int algo           = (int)s->alloc_algo;

    fwrite(&magic,      sizeof(int), 1, fp);
    fwrite(&version,    sizeof(int), 1, fp);
    fwrite(&pcb_count,  sizeof(int), 1, fp);
    fwrite(&s->next_pid,sizeof(int), 1, fp);
    fwrite(&clock,      sizeof(int), 1, fp);
    fwrite(&sched_run,  sizeof(int), 1, fp);
    fwrite(&algo,       sizeof(int), 1, fp);
    fwrite(&free_count, sizeof(int), 1, fp);
    fwrite(&alloc_count,sizeof(int), 1, fp);

    /* ---- Write PCB records ---- */
    PCB *p = s->process_list;
    while (p) {
        PCBRecord rec;
        pcb_to_record(p, &rec);
        fwrite(&rec, sizeof(PCBRecord), 1, fp);
        p = p->next;
    }

    /* ---- Write MLFQ state: PIDs per level in queue order ---- */
    for (int i = 0; i < MLFQ_LEVELS; i++) {
        MLFQ_Level *q = &s->scheduler.levels[i];
        fwrite(&q->count, sizeof(int), 1, fp);

        if (q->count > 0 && q->head) {
            PCB *cur = q->head;
            do {
                fwrite(&cur->pid, sizeof(int), 1, fp);
                cur = cur->queue_next;
            } while (cur != q->head);
        }
    }

    /* ---- Write free blocks ---- */
    fb = s->free_list;
    while (fb) {
        fwrite(&fb->start, sizeof(int), 1, fp);
        fwrite(&fb->size,  sizeof(int), 1, fp);
        fb = fb->next;
    }

    /* ---- Write allocated blocks ---- */
    ab = s->alloc_list;
    while (ab) {
        fwrite(&ab->start, sizeof(int), 1, fp);
        fwrite(&ab->size,  sizeof(int), 1, fp);
        fwrite(&ab->pid,   sizeof(int), 1, fp);
        ab = ab->next;
    }

    fclose(fp);
    s->dirty = 0;

    printf("[Save] State saved to %s\n", STATE_FILE);
    printf("  Processes: %d | Free blocks: %d | Alloc blocks: %d | Clock: %d\n",
           pcb_count, free_count, alloc_count, clock);
    return 0;
}

/* ================================================================
 * load — restore state from file
 * ================================================================ */

int persistence_load(SystemState *s) {
    FILE *fp = fopen(STATE_FILE, "rb");
    if (!fp) return 0;  /* cold start */

    /* ---- Read Header ---- */
    int magic, version, pcb_count, next_pid, clock;
    int sched_run, algo, free_count, alloc_count;

    if (fread(&magic,     sizeof(int), 1, fp) != 1) goto fail;
    if (fread(&version,   sizeof(int), 1, fp) != 1) goto fail;
    if (fread(&pcb_count, sizeof(int), 1, fp) != 1) goto fail;
    if (fread(&next_pid,  sizeof(int), 1, fp) != 1) goto fail;
    if (fread(&clock,     sizeof(int), 1, fp) != 1) goto fail;
    if (fread(&sched_run, sizeof(int), 1, fp) != 1) goto fail;
    if (fread(&algo,      sizeof(int), 1, fp) != 1) goto fail;
    if (fread(&free_count,sizeof(int), 1, fp) != 1) goto fail;
    if (fread(&alloc_count,sizeof(int),1, fp) != 1) goto fail;

    if (magic != OS_MAGIC) {
        fprintf(stderr, "[Warn] Magic mismatch in state file, cold start.\n");
        goto fail;
    }

    clear_state(s);

    /* ---- Read PCB records ---- */
    PCB **pdbuf = malloc(sizeof(PCB*) * pcb_count);
    if (!pdbuf) goto fail;

    for (int i = 0; i < pcb_count; i++) {
        PCBRecord rec;
        if (fread(&rec, sizeof(PCBRecord), 1, fp) != 1) {
            free(pdbuf);
            goto fail;
        }
        PCB *p = record_to_pcb(&rec);
        if (!p) { free(pdbuf); goto fail; }

        pdbuf[i] = p;

        p->next = s->process_list;
        s->process_list = p;
        s->process_count++;
    }

    /* Rebuild process tree (parent/child/sibling) */
    for (int i = 0; i < pcb_count; i++) {
        PCB *p = pdbuf[i];
        if (p->ppid > 0) {
            for (int j = 0; j < pcb_count; j++) {
                if (pdbuf[j]->pid == p->ppid) {
                    p->parent = pdbuf[j];
                    p->next_sibling = pdbuf[j]->first_child;
                    pdbuf[j]->first_child = p;
                    break;
                }
            }
        }
    }

    /* ---- Read MLFQ state ---- */
    for (int i = 0; i < MLFQ_LEVELS; i++) {
        MLFQ_Level *q = &s->scheduler.levels[i];
        int qcount;
        if (fread(&qcount, sizeof(int), 1, fp) != 1) { free(pdbuf); goto fail; }

        q->count = 0;
        q->head  = NULL;
        q->tail  = NULL;

        PCB *first = NULL, *prev = NULL;
        for (int j = 0; j < qcount; j++) {
            int qpid;
            if (fread(&qpid, sizeof(int), 1, fp) != 1) { free(pdbuf); goto fail; }

            PCB *found = NULL;
            for (int k = 0; k < pcb_count; k++) {
                if (pdbuf[k]->pid == qpid) { found = pdbuf[k]; break; }
            }
            if (!found) continue;

            found->queue_next = found->queue_prev = NULL;

            if (!first) {
                first = q->head = found;
                found->queue_next = found->queue_prev = found;
            } else {
                found->queue_prev = prev;
                found->queue_next = first;
                prev->queue_next = found;
                first->queue_prev = found;
            }
            prev = found;
            q->count++;
        }
        q->tail = prev;
    }

    free(pdbuf);

    /* ---- Read free blocks ---- */
    for (int i = 0; i < free_count; i++) {
        FreeBlock *fb = malloc(sizeof(FreeBlock));
        if (!fb) goto fail;
        if (fread(&fb->start, sizeof(int), 1, fp) != 1 ||
            fread(&fb->size,  sizeof(int), 1, fp) != 1) {
            free(fb);
            goto fail;
        }
        fb->next = s->free_list;
        s->free_list = fb;
    }

    /* ---- Read allocated blocks ---- */
    for (int i = 0; i < alloc_count; i++) {
        AllocBlock *ab = malloc(sizeof(AllocBlock));
        if (!ab) goto fail;
        if (fread(&ab->start, sizeof(int), 1, fp) != 1 ||
            fread(&ab->size,  sizeof(int), 1, fp) != 1 ||
            fread(&ab->pid,   sizeof(int), 1, fp) != 1) {
            free(ab);
            goto fail;
        }
        ab->next = s->alloc_list;
        s->alloc_list = ab;
    }

    /* ---- Restore system fields ---- */
    s->next_pid     = next_pid;
    s->system_clock = clock;
    s->sched_running = 0;
    s->sched_paused  = 0;
    s->alloc_algo   = (AllocAlgorithm)algo;
    s->running      = NULL;
    s->dirty        = 0;

    fclose(fp);

    printf("[Load] State restored from %s.\n", STATE_FILE);
    printf("  Processes: %d | Free blocks: %d | Alloc blocks: %d | Clock: %d\n",
           pcb_count, free_count, alloc_count, clock);
    return 1;

fail:
    fclose(fp);
    return -1;
}
