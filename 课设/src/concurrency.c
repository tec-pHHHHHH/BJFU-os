/**
 * concurrency.c — Multi-instance concurrency sharing
 *
 *   File lock: O_CREAT|O_EXCL based cross-instance mutual exclusion
 *   Reload daemon: polls state file mtime every RELOAD_INTERVAL_SEC seconds
 *   Auto-save: writes state on changes when autosave is enabled
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include "include/os_types.h"
#include "include/persistence.h"
#include "include/scheduler.h"
#include "include/concurrency.h"

static int lock_fd = -1;

/* ================================================================
 * File lock — based on O_CREAT|O_EXCL
 * ================================================================ */

int concurrency_lock(void) {
    int retries = 0;
    while (retries < 10) {
        lock_fd = open(LOCK_FILE, O_CREAT | O_EXCL | O_WRONLY, 0644);
        if (lock_fd >= 0) return 0;

        if (errno == EEXIST) {
            usleep(200000);
            retries++;
        } else {
            perror("concurrency_lock: open");
            return -1;
        }
    }
    fprintf(stderr, "[Warn] Failed to acquire file lock (%s)\n", LOCK_FILE);
    return -1;
}

void concurrency_unlock(void) {
    if (lock_fd >= 0) {
        close(lock_fd);
        lock_fd = -1;
    }
    remove(LOCK_FILE);
}

/* ================================================================
 * Auto-save — lock, write state file, update mtime
 * ================================================================ */

void concurrency_autosave(SystemState *s) {
    if (!s->autosave) return;
    if (!s->dirty) return;

    if (concurrency_lock() != 0) return;

    persistence_save(s);

    struct stat st;
    if (stat(STATE_FILE, &st) == 0) {
        s->state_file_mtime = st.st_mtime;
    }

    concurrency_unlock();
}

/* ================================================================
 * Reload daemon thread — polls state file, notifies on change
 * ================================================================ */

static void *reload_daemon_thread(void *arg) {
    SystemState *s = (SystemState *)arg;

    struct stat st;
    if (stat(STATE_FILE, &st) == 0) {
        s->state_file_mtime = st.st_mtime;
    }

    printf("[Daemon] Auto-reload thread started (interval=%ds).\n", RELOAD_INTERVAL_SEC);

    while (s->reload_running) {
        sleep(RELOAD_INTERVAL_SEC);
        if (!s->reload_running) break;

        if (stat(STATE_FILE, &st) != 0) {
            continue;
        }

        if (st.st_mtime != s->state_file_mtime) {
            s->state_file_mtime = st.st_mtime;

            int was_running = s->sched_running;
            int was_paused  = s->sched_paused;
            if (was_running && !was_paused) {
                pthread_mutex_lock(&s->sched_mutex);
                s->sched_paused = 1;
                pthread_mutex_unlock(&s->sched_mutex);
            }

            int ret = persistence_load(s);
            if (ret == 1) {
                printf("[Daemon] External update detected, state auto-synced.\n");
            }

            if (was_running && !was_paused) {
                pthread_mutex_lock(&s->sched_mutex);
                s->sched_paused = 0;
                pthread_cond_signal(&s->sched_cond);
                pthread_mutex_unlock(&s->sched_mutex);
            }
        }
    }

    printf("[Daemon] Auto-reload thread exited.\n");
    return NULL;
}

/* ================================================================
 * Start/stop daemon thread
 * ================================================================ */

int concurrency_start_daemon(SystemState *s) {
    if (s->reload_running) return 0;

    struct stat st;
    if (stat(STATE_FILE, &st) == 0) {
        s->state_file_mtime = st.st_mtime;
    } else {
        s->state_file_mtime = 0;
    }

    s->reload_running = 1;

    if (pthread_create(&s->reload_tid, NULL, reload_daemon_thread, s) != 0) {
        perror("pthread_create (reload_daemon)");
        s->reload_running = 0;
        return -1;
    }
    return 0;
}

void concurrency_stop_daemon(SystemState *s) {
    if (!s->reload_running) return;
    s->reload_running = 0;
    pthread_join(s->reload_tid, NULL);
}
