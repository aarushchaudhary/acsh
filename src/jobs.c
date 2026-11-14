#define _POSIX_C_SOURCE 200809L  /* strdup */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "acsh.h"

static Job job_table[ACSH_MAX_JOBS];

/* Set by the SIGCHLD handler when any job changed state. */
static volatile sig_atomic_t changes_pending = 0;
static int next_job_id = 1;

/* Frees a slot. Never called from the signal handler, which only reads
 * the table and sets state fields. */
static void job_free(Job *j) {
    free(j->pids);
    free(j->cmdline);
    j->pids = NULL;
    j->cmdline = NULL;
    j->in_use = 0;
    j->notify = 0;
}

int jobs_add(pid_t pgid, pid_t pids[], int num_pids, const char *cmdline) {
    for (int i = 0; i < ACSH_MAX_JOBS; i++) {
        Job *j = &job_table[i];
        if (j->in_use) {
            continue;
        }
        j->pids = malloc((size_t)num_pids * sizeof(pid_t));
        j->cmdline = strdup(cmdline);
        if (j->pids == NULL || j->cmdline == NULL) {
            job_free(j);
            fprintf(stderr, "acsh: out of memory\n");
            return -1;
        }
        memcpy(j->pids, pids, (size_t)num_pids * sizeof(pid_t));
        j->id = next_job_id++;
        j->pgid = pgid;
        j->num_pids = num_pids;
        j->state = JOB_RUNNING;
        j->notify = 0;
        j->remaining = num_pids;
        j->last_status = 0;
        j->in_use = 1; /* last: the handler only looks at slots in use */
        return j->id;
    }
    fprintf(stderr, "acsh: job table full\n");
    return -1;
}

static Job *find_by_pgid(pid_t pgid) {
    for (int i = 0; i < ACSH_MAX_JOBS; i++) {
        if (job_table[i].in_use && job_table[i].pgid == pgid) {
            return &job_table[i];
        }
    }
    return NULL;
}

static Job *find_by_pid(pid_t pid) {
    for (int i = 0; i < ACSH_MAX_JOBS; i++) {
        Job *j = &job_table[i];
        if (!j->in_use) continue;
        for (int k = 0; k < j->num_pids; k++) {
            if (j->pids[k] == pid) return j;
        }
    }
    return NULL;
}

void jobs_child_exited(pid_t pid, int status) {
    Job *j = find_by_pid(pid);
    if (j == NULL) {
        return;
    }
    if (pid == j->pids[j->num_pids - 1]) {
        j->last_status = status_to_exit_code(status);
    }
    if (--j->remaining <= 0 && j->state != JOB_DONE) {
        j->state = JOB_DONE;
        j->notify = 1;
        changes_pending = 1;
    }
}

void jobs_child_stopped(pid_t pid) {
    Job *j = find_by_pid(pid);
    if (j != NULL) {
        jobs_mark_stopped(j->pgid);
    }
}

void jobs_remove(Job *j) {
    job_free(j);
}

void jobs_mark_stopped(pid_t pgid) {
    Job *j = find_by_pgid(pgid);
    if (j != NULL) {
        j->state = JOB_STOPPED;
        j->notify = 1;
        changes_pending = 1;
    }
}

void jobs_report_changes(void) {
    if (!changes_pending) {
        return;
    }
    block_sigchld(); /* the handler must not change the table mid-scan */
    changes_pending = 0;
    for (int i = 0; i < ACSH_MAX_JOBS; i++) {
        Job *j = &job_table[i];
        if (!j->in_use || !j->notify) {
            continue;
        }
        j->notify = 0;
        if (shell_is_interactive()) {
            printf("[%d]+ %-22s%s\n", j->id,
                   j->state == JOB_DONE ? "Done" : "Stopped", j->cmdline);
        }
        if (j->state == JOB_DONE) {
            job_free(j);
        }
    }
    fflush(stdout);
    unblock_sigchld();
}

void jobs_mark_running(pid_t pgid) {
    Job *j = find_by_pgid(pgid);
    if (j != NULL) {
        j->state = JOB_RUNNING;
    }
}

void jobs_cleanup_done(void) {
    for (int i = 0; i < ACSH_MAX_JOBS; i++) {
        if (job_table[i].in_use && job_table[i].state == JOB_DONE) {
            job_free(&job_table[i]);
        }
    }
}

Job *jobs_find_by_id(int id) {
    for (int i = 0; i < ACSH_MAX_JOBS; i++) {
        if (job_table[i].in_use && job_table[i].id == id) {
            return &job_table[i];
        }
    }
    return NULL;
}

Job *jobs_find_most_recent(void) {
    Job *most_recent = NULL;
    for (int i = 0; i < ACSH_MAX_JOBS; i++) {
        if (job_table[i].in_use) {
            if (most_recent == NULL || job_table[i].id > most_recent->id) {
                most_recent = &job_table[i];
            }
        }
    }
    return most_recent;
}

void jobs_print_all(void) {
    for (int i = 0; i < ACSH_MAX_JOBS; i++) {
        if (!job_table[i].in_use) {
            continue;
        }
        const char *state_str =
        job_table[i].state == JOB_RUNNING ? "Running" :
        job_table[i].state == JOB_STOPPED ? "Stopped" : "Done";
        printf("[%d]  %-8s  %s\n", job_table[i].id, state_str, job_table[i].cmdline);
    }
}
