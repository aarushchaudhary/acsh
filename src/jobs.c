#include <stdio.h>
#include <string.h>
#include <signal.h>
#include "acsh.h"

static Job job_table[ACSH_MAX_JOBS];
static int next_job_id = 1;

int jobs_add(pid_t pgid, pid_t pids[], int num_pids, const char *cmdline) {
    for (int i = 0; i < ACSH_MAX_JOBS; i++) {
        if (!job_table[i].in_use) {
            job_table[i].in_use = 1;
            job_table[i].id = next_job_id++;
            job_table[i].pgid = pgid;
            job_table[i].num_pids = num_pids;
            for (int k = 0; k < num_pids; k++) {
                job_table[i].pids[k] = pids[k];
            }
            job_table[i].state = JOB_RUNNING;
            strncpy(job_table[i].cmdline, cmdline, ACSH_MAX_LINE - 1);
            job_table[i].cmdline[ACSH_MAX_LINE - 1] = '\0';
            return job_table[i].id;
        }
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

void jobs_mark_done(pid_t pgid) {
    Job *j = find_by_pgid(pgid);
    if (j == NULL) {
        return;
    }
    if (j->state != JOB_DONE) {
        j->state = JOB_DONE;
        /* only announce for background jobs -- foreground jobs are
         * already visibly done because the prompt returns */
        printf("[%d]+ Done                    %s\n", j->id, j->cmdline);
    }
}

void jobs_mark_stopped(pid_t pgid) {
    Job *j = find_by_pgid(pgid);
    if (j != NULL) {
        j->state = JOB_STOPPED;
        printf("[%d]+ Stopped                 %s\n", j->id, j->cmdline);
    }
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
            job_table[i].in_use = 0;
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
