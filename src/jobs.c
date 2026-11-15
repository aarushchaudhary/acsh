#define _POSIX_C_SOURCE 200809L  /* strdup, strsignal */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "acsh.h"

/* The job table: background and stopped jobs. The SIGCHLD handler only
 * updates state fields of slots in use (jobs_child_exited/stopped);
 * slots are added and freed outside the handler, with SIGCHLD blocked. */

static Job job_table[ACSH_MAX_JOBS];
static volatile sig_atomic_t changes_pending = 0;
static int sequence = 0;  /* recency: higher = more recently started or stopped */

static void job_free(Job *j) {
    free(j->pids);
    free(j->cmdline);
    j->pids = NULL;
    j->cmdline = NULL;
    j->in_use = 0;
    j->notify = 0;
}

/* The smallest job number no current job uses (as in ash and bash). */
static int lowest_free_id(void) {
    for (int id = 1;; id++) {
        int used = 0;
        for (int i = 0; i < ACSH_MAX_JOBS; i++) {
            if (job_table[i].in_use && job_table[i].id == id) used = 1;
        }
        if (!used) return id;
    }
}

int jobs_add(pid_t pgid, pid_t pids[], int num_pids, const char *cmdline) {
    for (int i = 0; i < ACSH_MAX_JOBS; i++) {
        Job *j = &job_table[i];
        if (j->in_use) continue;
        j->pids = malloc((size_t)num_pids * sizeof(pid_t));
        j->cmdline = strdup(cmdline);
        if (j->pids == NULL || j->cmdline == NULL) {
            job_free(j);
            shell_error("out of memory\n");
            return -1;
        }
        memcpy(j->pids, pids, (size_t)num_pids * sizeof(pid_t));
        j->id = lowest_free_id();
        j->pgid = pgid;
        j->num_pids = num_pids;
        j->state = JOB_RUNNING;
        j->notify = 0;
        j->remaining = num_pids;
        j->last_status = 0;
        j->signaled = 0;
        j->has_tmodes = 0;
        j->seq = ++sequence;
        j->in_use = 1; /* last: the handler only looks at slots in use */
        return j->id;
    }
    shell_error("job table full\n");
    return -1;
}

static Job *find_by_pgid(pid_t pgid) {
    for (int i = 0; i < ACSH_MAX_JOBS; i++) {
        if (job_table[i].in_use && job_table[i].pgid == pgid) return &job_table[i];
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

Job *jobs_find_by_pid(pid_t pid) {
    return find_by_pid(pid);
}

Job *jobs_find_by_id(int id) {
    for (int i = 0; i < ACSH_MAX_JOBS; i++) {
        if (job_table[i].in_use && job_table[i].id == id) return &job_table[i];
    }
    return NULL;
}

Job *jobs_at(int index) {
    if (index < 0 || index >= ACSH_MAX_JOBS || !job_table[index].in_use) return NULL;
    return &job_table[index];
}

void jobs_child_exited(pid_t pid, int status) {
    Job *j = find_by_pid(pid);
    if (j == NULL) return;
    if (pid == j->pids[j->num_pids - 1]) {
        j->last_status = status_to_exit_code(status);
        j->signaled = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    }
    if (--j->remaining <= 0 && j->state != JOB_DONE) {
        j->state = JOB_DONE;
        j->notify = 1;
        changes_pending = 1;
    }
}

void jobs_child_stopped(pid_t pid) {
    Job *j = find_by_pid(pid);
    if (j != NULL) jobs_mark_stopped(j->pgid);
}

void jobs_mark_stopped(pid_t pgid) {
    Job *j = find_by_pgid(pgid);
    if (j != NULL) {
        j->state = JOB_STOPPED;
        j->seq = ++sequence;
        j->notify = 1;
        changes_pending = 1;
    }
}

void jobs_mark_running(pid_t pgid) {
    Job *j = find_by_pgid(pgid);
    if (j != NULL) j->state = JOB_RUNNING;
}

void jobs_remove(Job *j) {
    job_free(j);
}

/* The current job (%+): the most recent stopped job, else the most
 * recent job. The previous job (%-) is the next one by the same order. */
static int ranks_above(const Job *a, const Job *b) {
    if (b == NULL) return 1;
    int as = (a->state == JOB_STOPPED), bs = (b->state == JOB_STOPPED);
    if (as != bs) return as;
    return a->seq > b->seq;
}

static Job *ranked(int rank) {
    Job *first = NULL, *second = NULL;
    for (int i = 0; i < ACSH_MAX_JOBS; i++) {
        Job *j = &job_table[i];
        if (!j->in_use) continue;
        if (ranks_above(j, first)) {
            second = first;
            first = j;
        } else if (ranks_above(j, second)) {
            second = j;
        }
    }
    return rank == 0 ? first : second;
}

Job *jobs_find_most_recent(void) {
    return ranked(0);
}

Job *jobs_resolve(const char *spec) {
    if (spec == NULL || strcmp(spec, "%") == 0 || strcmp(spec, "%%") == 0 || strcmp(spec, "%+") == 0) {
        return ranked(0);
    }
    if (strcmp(spec, "%-") == 0) {
        return ranked(1);
    }
    const char *s = (spec[0] == '%') ? spec + 1 : spec;
    if (*s >= '0' && *s <= '9') {
        return jobs_find_by_id(atoi(s));
    }
    int contains = (*s == '?');
    if (contains) s++;
    Job *found = NULL;
    for (int i = 0; i < ACSH_MAX_JOBS; i++) {
        Job *j = &job_table[i];
        if (!j->in_use) continue;
        int match = contains ? strstr(j->cmdline, s) != NULL : strncmp(j->cmdline, s, strlen(s)) == 0;
        if (match) {
            if (found != NULL) return NULL; /* ambiguous */
            found = j;
        }
    }
    return found;
}

/* "Running", "Stopped", "Done", "Done(N)", or the signal ("Terminated"). */
static const char *status_text(Job *j, char *buf, size_t size) {
    if (j->state == JOB_RUNNING) return "Running";
    if (j->state == JOB_STOPPED) return "Stopped";
    if (j->signaled) return strsignal(j->signaled);
    if (j->last_status == 0) return "Done";
    snprintf(buf, size, "Done(%d)", j->last_status);
    return buf;
}

/* One line in ash's layout: "[1]+  Running                    cmd". */
static void print_job(Job *j, int with_pid) {
    char buf[32], field[64];
    char mark = (j == ranked(0)) ? '+' : (j == ranked(1)) ? '-' : ' ';
    const char *st = status_text(j, buf, sizeof(buf));
    if (with_pid) {
        snprintf(field, sizeof(field), "%d %s", (int)j->pgid, st);
    } else {
        snprintf(field, sizeof(field), "%s", st);
    }
    printf("[%d]%c  %-27s%s\n", j->id, mark, field, j->cmdline);
}

void jobs_report_changes(void) {
    if (!changes_pending) return;
    block_sigchld(); /* the handler must not change the table mid-scan */
    changes_pending = 0;
    for (int i = 0; i < ACSH_MAX_JOBS; i++) {
        Job *j = &job_table[i];
        if (!j->in_use || !j->notify) continue;
        j->notify = 0;
        if (shell_is_interactive()) print_job(j, 0);
        if (j->state == JOB_DONE) job_free(j);
    }
    fflush(stdout);
    unblock_sigchld();
}

void jobs_cleanup_done(void) {
    for (int i = 0; i < ACSH_MAX_JOBS; i++) {
        if (job_table[i].in_use && job_table[i].state == JOB_DONE) job_free(&job_table[i]);
    }
}

void jobs_print_all(int with_pid, int pids_only) {
    block_sigchld();
    Job *order[ACSH_MAX_JOBS];
    int n = 0;
    Job *cur = ranked(0), *prev = ranked(1);
    if (cur) order[n++] = cur;
    if (prev) order[n++] = prev;
    for (int i = 0; i < ACSH_MAX_JOBS; i++) {
        Job *j = &job_table[i];
        if (j->in_use && j != cur && j != prev) order[n++] = j;
    }
    for (int k = 0; k < n; k++) {
        if (pids_only) printf("%d\n", (int)order[k]->pgid);
        else print_job(order[k], with_pid);
        order[k]->notify = 0;
    }
    fflush(stdout);
    jobs_cleanup_done(); /* finished jobs are shown once, then forgotten */
    unblock_sigchld();
}
