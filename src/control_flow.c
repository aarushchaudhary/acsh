#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fnmatch.h>
#include "acsh.h"

/* Signals "stop running further statements" up through run_statements()
 * and its callers, for the `break` and `continue` builtins. Modeled
 * as a small piece of module-level state (not a return value threaded
 * through every function) because break/continue must propagate
 * through run_if_block()/run_case_block() UNCHANGED -- e.g. `break`
 * inside an `if` inside a `while` must stop the while loop, not just
 * the if -- and threading an extra out-parameter through every one of
 * this file's mutually-recursive functions would make the existing
 * control flow much harder to follow for comparatively little safety
 * benefit in a single-threaded interpreter like this one.
 *
 * loop_signal_level counts how many enclosing loops the break/continue
 * should skip past: `break` = level 1 (the innermost loop), `break 2`
 * = level 2 (skip out of two nested loops), etc., matching POSIX. Each
 * loop construct (run_loop_block for while/until, run_for_block for
 * for) checks for a pending signal after running its body, and if the
 * signal's level applies to IT, consumes one level and acts (stops
 * for break, skips to the next iteration for continue); otherwise it
 * leaves the signal in place for an enclosing loop to consume. */
typedef enum {
    LOOP_SIGNAL_NONE,
    LOOP_SIGNAL_BREAK,
    LOOP_SIGNAL_CONTINUE
} LoopSignal;

static LoopSignal loop_signal = LOOP_SIGNAL_NONE;
static int loop_signal_level = 0;
static int active_loop_depth = 0; /* how many while/until/for loops are currently running, nested */

/* Called by the `break`/`continue` builtins (builtins.c) to raise the
 * signal. `level` is the N in `break N` / `continue N` (1 if bare). */
void control_flow_signal_loop(int is_continue, int level) {
    loop_signal = is_continue ? LOOP_SIGNAL_CONTINUE : LOOP_SIGNAL_BREAK;
    loop_signal_level = (level >= 1) ? level : 1;
}

/* Returns 1 if there is currently no pending break/continue outside a
 * loop -- used by the `break`/`continue` builtins themselves to warn
 * if they're invoked outside any loop (matching real shells, which
 * print a warning but don't treat it as a hard error). */
int control_flow_in_loop_depth(void) {
    return active_loop_depth;
}

/* Returns 1 if `word` (of length `len`) exactly matches `kw` as a
 * whole word -- i.e. not a prefix of a longer identifier like "iffy"
 * or "thenable". Used to find if/then/elif/else/fi as keywords rather
 * than matching them inside other words. */
static int word_matches(const char *word, size_t len, const char *kw) {
    return strlen(kw) == len && strncmp(word, kw, len) == 0;
}

int is_if_statement(const char *line) {
    while (*line == ' ' || *line == '\t') line++;
    size_t i = 0;
    while (line[i] != '\0' && line[i] != ' ' && line[i] != '\t' &&
        line[i] != ';' && line[i] != '\n') {
        i++;
        }
        return word_matches(line, i, "if");
}

/* Bounded version of is_if_statement(), for text that isn't
 * NUL-terminated at a convenient point (spans inside a larger
 * buffer). Returns 1 if the first word in [p, end) is "if". */
static int starts_with_if(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    size_t i = 0;
    while (p + i < end && p[i] != ' ' && p[i] != '\t' &&
        p[i] != ';' && p[i] != '\n') {
        i++;
        }
        return word_matches(p, i, "if");
}

int is_while_statement(const char *line) {
    while (*line == ' ' || *line == '\t') line++;
    size_t i = 0;
    while (line[i] != '\0' && line[i] != ' ' && line[i] != '\t' &&
        line[i] != ';' && line[i] != '\n') {
        i++;
        }
        return word_matches(line, i, "while");
}

int is_until_statement(const char *line) {
    while (*line == ' ' || *line == '\t') line++;
    size_t i = 0;
    while (line[i] != '\0' && line[i] != ' ' && line[i] != '\t' &&
        line[i] != ';' && line[i] != '\n') {
        i++;
        }
        return word_matches(line, i, "until");
}

int is_for_statement(const char *line) {
    while (*line == ' ' || *line == '\t') line++;
    size_t i = 0;
    while (line[i] != '\0' && line[i] != ' ' && line[i] != '\t' &&
        line[i] != ';' && line[i] != '\n') {
        i++;
        }
        return word_matches(line, i, "for");
}

int is_case_statement(const char *line) {
    while (*line == ' ' || *line == '\t') line++;
    size_t i = 0;
    while (line[i] != '\0' && line[i] != ' ' && line[i] != '\t' &&
        line[i] != ';' && line[i] != '\n') {
        i++;
        }
        return word_matches(line, i, "case");
}

/* Returns "while" or "until" if [p, end)'s first word is one of them,
 * else NULL. Used by run_statements() to detect a nested loop and
 * find its keyword length (5 either way, conveniently, but kept
 * explicit rather than hardcoded for clarity). */
static const char *starts_with_while_or_until(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    size_t i = 0;
    while (p + i < end && p[i] != ' ' && p[i] != '\t' &&
        p[i] != ';' && p[i] != '\n') {
        i++;
        }
        if (word_matches(p, i, "while")) return "while";
        if (word_matches(p, i, "until")) return "until";
        return NULL;
}

/* Given `p` pointing at the "case" keyword itself, returns a pointer
 * just past that case block's own matching "esac" -- treating the
 * whole case...esac construct as OPAQUE to the OTHER constructs'
 * keyword scanners (find_next_keyword for if/fi, find_next_do_done
 * for while/until/for/done). This is necessary because case bodies
 * routinely contain patterns, `)`, and ";;" that would otherwise
 * confuse a naive depth counter -- rather than teach every other
 * scanner full case-pattern syntax, they call this to jump over a
 * nested case block in one step. Returns NULL if no matching "esac"
 * is found before `limit`. A nested case...esac inside THIS case
 * block is handled by the same depth counter, matching how if/fi and
 * do/done nesting is handled elsewhere in this file. */
static const char *skip_case_block(const char *p, const char *limit) {
    const char *q = p + 4; /* skip "case" */
    char in_quote = '\0';
    int depth = 0;

    while (q < limit) {
        if (in_quote != '\0') {
            if (*q == in_quote) in_quote = '\0';
            q++;
            continue;
        }
        if (*q == '\'' || *q == '"') { in_quote = *q; q++; continue; }

        int at_word_start = (q == p + 4) || (q[-1] == ' ' || q[-1] == '\t' ||
        q[-1] == '\n' || q[-1] == ';' ||
        q[-1] == ')' || q[-1] == '(');
        if (at_word_start) {
            size_t wlen = 0;
            while (q + wlen < limit && q[wlen] != ' ' && q[wlen] != '\t' &&
                q[wlen] != '\n' && q[wlen] != ';' && q[wlen] != ')' &&
                q[wlen] != '(') {
                wlen++;
                }
                if (wlen > 0 && word_matches(q, wlen, "case")) {
                    depth++;
                } else if (wlen > 0 && word_matches(q, wlen, "esac")) {
                    if (depth > 0) {
                        depth--;
                    } else {
                        return q + 4; /* just past this "esac" */
                    }
                }
                q += (wlen == 0) ? 1 : wlen;
                continue;
        }
        q++;
    }

    return NULL;
}

/* Quote-aware, nesting-aware scan (bounded to [buf, limit)) that
 * finds the next occurrence of one of the control-flow keywords
 * (then/elif/else/fi) as a whole word, outside any quotes, belonging
 * to the SAME if-block that starts at `buf` -- any "if" encountered
 * along the way opens a nested block (depth++) whose own "fi" is
 * consumed (depth--) without being mistaken for the keyword we're
 * looking for. This is what makes nested if/fi blocks resolve their
 * boundaries correctly. Returns NULL if none found before `limit`. */
static const char *find_next_keyword(const char *buf, const char *limit, const char **out_kw) {
    const char *p = buf;
    char in_quote = '\0';
    int depth = 0;

    while (p < limit) {
        if (in_quote != '\0') {
            if (*p == in_quote) in_quote = '\0';
            p++;
            continue;
        }
        if (*p == '\'' || *p == '"') {
            in_quote = *p;
            p++;
            continue;
        }

        int at_word_start = (p == buf) || (p[-1] == ' ' || p[-1] == '\t' ||
        p[-1] == '\n' || p[-1] == ';');
        if (at_word_start) {
            size_t wlen = 0;
            while (p + wlen < limit && p[wlen] != ' ' && p[wlen] != '\t' &&
                p[wlen] != '\n' && p[wlen] != ';') {
                wlen++;
                }

                if (wlen > 0 && word_matches(p, wlen, "case")) {
                    const char *after_esac = skip_case_block(p, limit);
                    if (after_esac == NULL) {
                        return NULL; /* malformed nested case, let caller report it */
                    }
                    p = after_esac;
                    continue;
                }

                if (wlen > 0 && word_matches(p, wlen, "if")) {
                    depth++;
                } else if (wlen > 0 && word_matches(p, wlen, "fi")) {
                    if (depth > 0) {
                        depth--;
                    } else {
                        *out_kw = "fi";
                        return p;
                    }
                } else if (depth == 0 && wlen > 0 &&
                    (word_matches(p, wlen, "then") ||
                    word_matches(p, wlen, "elif") ||
                    word_matches(p, wlen, "else"))) {
                    if (word_matches(p, wlen, "then")) *out_kw = "then";
                    else if (word_matches(p, wlen, "elif")) *out_kw = "elif";
                    else *out_kw = "else";
                    return p;
                    }

                    p += (wlen == 0) ? 1 : wlen;
                continue;
        }

        p++;
    }

    return NULL;
}

/* Returns 1 if a depth-0 "fi" exists anywhere in [buf, buf+len). Used
 * by run_if_statement() to know when enough lines have been read in
 * from an interactive/script source to fully close the (possibly
 * multi-line) if block. */
static int has_top_level_fi(const char *buf, size_t len) {
    const char *limit = buf + len;
    char in_quote = '\0';
    int depth = 0;
    const char *p = buf;

    while (p < limit) {
        if (in_quote != '\0') {
            if (*p == in_quote) in_quote = '\0';
            p++;
            continue;
        }
        if (*p == '\'' || *p == '"') { in_quote = *p; p++; continue; }

        int at_word_start = (p == buf) || (p[-1] == ' ' || p[-1] == '\t' ||
        p[-1] == '\n' || p[-1] == ';');
        if (at_word_start) {
            size_t wlen = 0;
            while (p + wlen < limit && p[wlen] != ' ' && p[wlen] != '\t' &&
                p[wlen] != '\n' && p[wlen] != ';') wlen++;
            if (wlen > 0 && word_matches(p, wlen, "case")) {
                const char *after_esac = skip_case_block(p, limit);
                if (after_esac == NULL) {
                    /* case block not yet closed within what we've
                     * read so far -- this just means more lines are
                     * needed (normal during line accumulation), not a
                     * real error, so report "no top-level fi yet"
                     * rather than skipping past the end of the buffer */
                    return 0;
                }
                p = after_esac;
                continue;
            }
            if (wlen > 0 && word_matches(p, wlen, "if")) {
                depth++;
            } else if (wlen > 0 && word_matches(p, wlen, "fi")) {
                if (depth > 0) depth--;
                else return 1;
            }
            p += (wlen == 0) ? 1 : wlen;
            continue;
        }
        p++;
    }
    return 0;
}

/* Same quote-aware, nesting-aware scanning technique as
 * find_next_keyword() above, generalized for while/until blocks:
 * finds the next occurrence of "do" or "done" as a whole word,
 * treating any nested "while"/"until"/"if" as opening a block whose
 * own closer must be consumed first. Needed because while/until use
 * do/done instead of then/fi, and can nest inside if blocks and vice
 * versa (e.g. an if body containing a while loop). */
static const char *find_next_do_done(const char *buf, const char *limit, const char **out_kw) {
    const char *p = buf;
    char in_quote = '\0';
    int depth = 0;

    while (p < limit) {
        if (in_quote != '\0') {
            if (*p == in_quote) in_quote = '\0';
            p++;
            continue;
        }
        if (*p == '\'' || *p == '"') {
            in_quote = *p;
            p++;
            continue;
        }

        int at_word_start = (p == buf) || (p[-1] == ' ' || p[-1] == '\t' ||
        p[-1] == '\n' || p[-1] == ';');
        if (at_word_start) {
            size_t wlen = 0;
            while (p + wlen < limit && p[wlen] != ' ' && p[wlen] != '\t' &&
                p[wlen] != '\n' && p[wlen] != ';') {
                wlen++;
                }

                if (wlen > 0 && word_matches(p, wlen, "case")) {
                    const char *after_esac = skip_case_block(p, limit);
                    if (after_esac == NULL) {
                        return NULL;
                    }
                    p = after_esac;
                    continue;
                }

                if (wlen > 0 && (word_matches(p, wlen, "while") ||
                    word_matches(p, wlen, "until") ||
                    word_matches(p, wlen, "for") ||
                    word_matches(p, wlen, "if"))) {
                    depth++;
                    } else if (wlen > 0 && (word_matches(p, wlen, "done") ||
                        word_matches(p, wlen, "fi"))) {
                        if (depth > 0) {
                            depth--;
                        } else if (word_matches(p, wlen, "done")) {
                            *out_kw = "done";
                            return p;
                        }
                        /* a stray "fi" at depth 0 here is a different
                         * construct's closer, not ours -- ignore it and keep
                         * scanning, matching real shells' independent nesting */
                        } else if (depth == 0 && wlen > 0 && word_matches(p, wlen, "do")) {
                            *out_kw = "do";
                            return p;
                        }

                        p += (wlen == 0) ? 1 : wlen;
                        continue;
        }

        p++;
    }

    return NULL;
}

/* Returns 1 if a depth-0 "done" exists anywhere in [buf, buf+len).
 * Mirrors has_top_level_fi() but for while/until...done blocks. */
static int has_top_level_done(const char *buf, size_t len) {
    const char *limit = buf + len;
    char in_quote = '\0';
    int depth = 0;
    const char *p = buf;

    while (p < limit) {
        if (in_quote != '\0') {
            if (*p == in_quote) in_quote = '\0';
            p++;
            continue;
        }
        if (*p == '\'' || *p == '"') { in_quote = *p; p++; continue; }

        int at_word_start = (p == buf) || (p[-1] == ' ' || p[-1] == '\t' ||
        p[-1] == '\n' || p[-1] == ';');
        if (at_word_start) {
            size_t wlen = 0;
            while (p + wlen < limit && p[wlen] != ' ' && p[wlen] != '\t' &&
                p[wlen] != '\n' && p[wlen] != ';') wlen++;
            if (wlen > 0 && word_matches(p, wlen, "case")) {
                const char *after_esac = skip_case_block(p, limit);
                if (after_esac == NULL) {
                    return 0; /* case not yet closed -- need more input */
                }
                p = after_esac;
                continue;
            }
            if (wlen > 0 && (word_matches(p, wlen, "while") ||
                word_matches(p, wlen, "until") ||
                word_matches(p, wlen, "for") ||
                word_matches(p, wlen, "if"))) {
                depth++;
                } else if (wlen > 0 && (word_matches(p, wlen, "done") ||
                    word_matches(p, wlen, "fi"))) {
                    if (depth > 0) depth--;
                    else if (word_matches(p, wlen, "done")) return 1;
                    }
                    p += (wlen == 0) ? 1 : wlen;
                continue;
        }
        p++;
    }
    return 0;
}

static int run_if_block(const char *start, const char *end);
static int run_loop_block(const char *start, const char *end, int is_until);
static int run_for_block(const char *start, const char *end);
static int run_case_block(const char *start, const char *end);

/* Runs the text in [start, end) as a sequence of statements, each
 * either an ordinary &&/||/; chain or -- if it begins with "if" -- a
 * nested if/then/elif/else/fi block, handled recursively via
 * run_if_block(). This recursive dispatch is what makes nested if
 * blocks work: without it, an outer if's body containing another
 * if/fi would be flattened into one Chain, and "if"/"then"/"fi" would
 * reach execute_chain() as literal (and invalid) command names.
 * Returns the exit status of the last statement actually run. */
static int run_statements(const char *start, const char *end) {
    const char *p = start;
    int status = 0;

    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == ';')) {
            p++;
        }
        if (p >= end) {
            break;
        }

        /* If a break/continue was raised by whatever statement just
         * ran (checked at the TOP of each iteration so it takes
         * effect immediately after any of the four construct types
         * below, or after an ordinary command), stop running further
         * statements in THIS span and let it propagate up. The
         * signal itself is left untouched here -- only run_loop_block()
         * and run_for_block() (the actual loop constructs) are
         * allowed to consume it; run_if_block()/run_case_block() and
         * this function all just stop early and pass it through. */
        if (loop_signal != LOOP_SIGNAL_NONE) {
            break;
        }

        if (starts_with_if(p, end)) {
            const char *kw;
            const char *fi_at = NULL;
            const char *q = p + 2; /* just past this block's own "if" */
            while (1) {
                const char *found = find_next_keyword(q, end, &kw);
                if (found == NULL) {
                    break;
                }
                if (strcmp(kw, "fi") == 0) {
                    fi_at = found;
                    break;
                }
                q = found + strlen(kw);
            }

            if (fi_at == NULL) {
                fprintf(stderr, "acsh: syntax error: unterminated 'if' (missing 'fi')\n");
                return 1;
            }

            status = run_if_block(p, fi_at + 2 /* include "fi" */);
            p = fi_at + 2;
            continue;
        }

        {
            const char *loop_kw = starts_with_while_or_until(p, end);
            if (loop_kw != NULL) {
                int is_until_loop = (strcmp(loop_kw, "until") == 0);
                const char *kw;
                const char *done_at = NULL;
                const char *q = p + strlen(loop_kw); /* just past this loop's own opener */
                while (1) {
                    const char *found = find_next_do_done(q, end, &kw);
                    if (found == NULL) {
                        break;
                    }
                    if (strcmp(kw, "done") == 0) {
                        done_at = found;
                        break;
                    }
                    q = found + strlen(kw);
                }

                if (done_at == NULL) {
                    fprintf(stderr, "acsh: syntax error: unterminated '%s' (missing 'done')\n", loop_kw);
                    return 1;
                }

                status = run_loop_block(p, done_at + 4 /* include "done" */, is_until_loop);
                p = done_at + 4;
                continue;
            }
        }

        if (p + 3 <= end && word_matches(p, 3, "for") &&
            (p + 3 == end || p[3] == ' ' || p[3] == '\t' || p[3] == '\n' || p[3] == ';')) {
            const char *kw;
        const char *done_at = NULL;
        const char *q = p + 3; /* just past this loop's own "for" */
        while (1) {
            const char *found = find_next_do_done(q, end, &kw);
            if (found == NULL) {
                break;
            }
            if (strcmp(kw, "done") == 0) {
                done_at = found;
                break;
            }
            q = found + strlen(kw);
        }

        if (done_at == NULL) {
            fprintf(stderr, "acsh: syntax error: unterminated 'for' (missing 'done')\n");
            return 1;
        }

        status = run_for_block(p, done_at + 4);
        p = done_at + 4;
        continue;
            }

            if (p + 4 <= end && word_matches(p, 4, "case") &&
                (p + 4 == end || p[4] == ' ' || p[4] == '\t' || p[4] == '\n' || p[4] == ';')) {
                const char *after_esac = skip_case_block(p, end);
            if (after_esac == NULL) {
                fprintf(stderr, "acsh: syntax error: unterminated 'case' (missing 'esac')\n");
                return 1;
            }
            status = run_case_block(p, after_esac);
            p = after_esac;
            continue;
                }

                /* plain statement: runs until the next physical newline
                 * (outside quotes), same as a normal input line */
                const char *line_end = p;
                char in_quote = '\0';
                while (line_end < end) {
                    if (in_quote != '\0') {
                        if (*line_end == in_quote) in_quote = '\0';
                        line_end++;
                        continue;
                    }
                    if (*line_end == '\'' || *line_end == '"') {
                        in_quote = *line_end;
                        line_end++;
                        continue;
                    }
                    if (*line_end == '\n') {
                        break;
                    }
                    line_end++;
                }

                size_t seg_len = (size_t)(line_end - p);
                char *text = malloc(seg_len + 1);
                if (text != NULL) {
                    memcpy(text, p, seg_len);
                    text[seg_len] = '\0';

                    Chain ch;
                    if (parse_chain(text, &ch) == 0 && ch.num_segments > 0) {
                        execute_chain(&ch);
                        status = env_get_last_status();
                    }
                    free(text);
                }

                p = line_end;
    }

    return status;
}

/* Runs one complete if/then/elif/else/fi block spanning [start, end),
 * where `start` points at the "if" keyword and `end` points just past
 * the closing "fi". Shared by the top-level entry point
 * (run_if_statement) and by run_statements() for nested blocks, so
 * both go through identical clause-walking logic. */
static int run_if_block(const char *start, const char *end) {
    const char *cursor = start + 2; /* skip leading "if" */
    int executed_a_branch = 0;
    int result_status = 0;

    while (1) {
        const char *kw;
        const char *then_pos = find_next_keyword(cursor, end, &kw);
        if (then_pos == NULL || strcmp(kw, "then") != 0) {
            fprintf(stderr, "acsh: syntax error: expected 'then'\n");
            return 1;
        }

        const char *cond_start = cursor;
        const char *cond_end = then_pos;
        cursor = then_pos + 4; /* skip "then" */

        const char *body_start = cursor;
        const char *stop = find_next_keyword(cursor, end, &kw);
        if (stop == NULL) {
            fprintf(stderr, "acsh: syntax error: expected 'elif', 'else', or 'fi'\n");
            return 1;
        }
        const char *body_end = stop;

        int cond_status = executed_a_branch ? 1 : run_statements(cond_start, cond_end);
        int should_run_this_branch = !executed_a_branch && (cond_status == 0);

        if (should_run_this_branch) {
            result_status = run_statements(body_start, body_end);
            executed_a_branch = 1;
        }

        cursor = stop;
        if (strcmp(kw, "fi") == 0) {
            break;
        }
        if (strcmp(kw, "else") == 0) {
            cursor += 4; /* skip "else" */
            const char *else_body_start = cursor;
            const char *fi_kw;
            const char *fi_pos_local = find_next_keyword(cursor, end, &fi_kw);
            if (fi_pos_local == NULL || strcmp(fi_kw, "fi") != 0) {
                fprintf(stderr, "acsh: syntax error: expected 'fi'\n");
                return 1;
            }
            if (!executed_a_branch) {
                result_status = run_statements(else_body_start, fi_pos_local);
                executed_a_branch = 1;
            }
            break;
        }
        if (strcmp(kw, "elif") == 0) {
            cursor += 4; /* skip "elif", loop continues to its condition */
            continue;
        }
    }

    return result_status;
}

/* Runs one complete while/until...do...done loop spanning [start,
 * end), where `start` points at the "while"/"until" keyword and `end`
 * points just past the closing "done". The condition is re-evaluated
 * and the body re-run each iteration, so side effects from the body
 * (e.g. incrementing a counter via `export i=$((i+1))`-style
 * commands, once arithmetic exists, or more simply anything that
 * changes state `test`/external commands can observe) are visible to
 * the NEXT condition check -- exactly like a real shell loop.
 * `is_until` inverts the stopping condition: while loops while the
 * condition succeeds (status 0); until loops while it FAILS. Capped
 * at ACSH_MAX_LOOP_ITERATIONS as a safety net against a condition
 * that can never become false (e.g. `while true; do ...; done` with
 * no break), so a scripting mistake can't hang the whole shell
 * forever -- acsh has no `break`/Ctrl+C-during-loop handling yet, so
 * this cap is the only guard against that case. */
static int run_loop_block(const char *start, const char *end, int is_until) {
    const char *opener_word = is_until ? "until" : "while";
    const char *cursor = start + strlen(opener_word);

    const char *kw;
    const char *do_pos = find_next_do_done(cursor, end, &kw);
    if (do_pos == NULL || strcmp(kw, "do") != 0) {
        fprintf(stderr, "acsh: syntax error: expected 'do'\n");
        return 1;
    }

    const char *cond_start = cursor;
    const char *cond_end = do_pos;
    const char *body_start = do_pos + 2; /* skip "do" */

    const char *done_kw;
    const char *done_pos = find_next_do_done(body_start, end, &done_kw);
    if (done_pos == NULL || strcmp(done_kw, "done") != 0) {
        fprintf(stderr, "acsh: syntax error: expected 'done'\n");
        return 1;
    }
    const char *body_end = done_pos;

    int result_status = 0;
    int iterations = 0;

    active_loop_depth++;

    while (1) {
        int cond_status = run_statements(cond_start, cond_end);
        int should_continue = is_until ? (cond_status != 0) : (cond_status == 0);
        if (!should_continue) {
            break;
        }

        iterations++;
        if (iterations > ACSH_MAX_LOOP_ITERATIONS) {
            fprintf(stderr, "acsh: %s loop exceeded %d iterations, stopping "
            "(safety limit -- check your loop condition)\n",
                    opener_word, ACSH_MAX_LOOP_ITERATIONS);
            break;
        }

        result_status = run_statements(body_start, body_end);

        /* Consume a pending break/continue if it targets THIS loop.
         * Level 1 always means "this loop"; a level > 1 means it's
         * meant for an enclosing loop further out, so decrement it by
         * one and leave it pending for that loop to catch instead. */
        if (loop_signal != LOOP_SIGNAL_NONE) {
            if (loop_signal_level <= 1) {
                LoopSignal sig = loop_signal;
                loop_signal = LOOP_SIGNAL_NONE;
                loop_signal_level = 0;
                if (sig == LOOP_SIGNAL_BREAK) {
                    break;
                }
                /* LOOP_SIGNAL_CONTINUE: fall through to re-check the
                 * condition and start the next iteration normally */
            } else {
                loop_signal_level--;
                break; /* propagate up to the enclosing loop */
            }
        }
    }

    active_loop_depth--;
    return result_status;
}

/* Runs a full while/until...do...done loop construct. Mirrors
 * run_if_statement()'s line-accumulation strategy (read more lines
 * from `read_more_line` until a top-level "done" exists), then hands
 * off to run_loop_block() for the actual repeated execution. */
int run_loop_statement(const char *first_line, int is_until,
                       char *(*read_more_line)(char *buf, int size)) {
    const char *opener = is_until ? "until" : "while";
    size_t opener_len = strlen(opener);

    size_t cap = ACSH_MAX_LINE * 4;
    char *buf = malloc(cap);
    if (buf == NULL) {
        fprintf(stderr, "acsh: out of memory\n");
        return 1;
    }
    size_t len = 0;

    {
        size_t plen = strlen(first_line);
        while (len + plen + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        memcpy(buf + len, first_line, plen);
        len += plen;
        buf[len++] = '\n';
    }

    while (!has_top_level_done(buf + opener_len, len - opener_len)) {
        char linebuf[ACSH_MAX_LINE];
        if (read_more_line(linebuf, sizeof(linebuf)) == NULL) {
            fprintf(stderr, "acsh: syntax error: unexpected end of input, expected 'done'\n");
            free(buf);
            return 1;
        }
        size_t llen = strlen(linebuf);
        while (len + llen + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
        memcpy(buf + len, linebuf, llen);
        len += llen;
        if (len == 0 || buf[len - 1] != '\n') {
            buf[len++] = '\n';
        }
    }

    const char *kw;
    const char *scan = buf + opener_len;
    const char *done_at = NULL;
    while (1) {
        const char *found = find_next_do_done(scan, buf + len, &kw);
        if (found == NULL) {
            break; /* shouldn't happen, has_top_level_done already confirmed one exists */
        }
        if (strcmp(kw, "done") == 0) {
            done_at = found;
            break;
        }
        scan = found + strlen(kw);
    }

    int status = 1;
    if (done_at != NULL) {
        status = run_loop_block(buf, done_at + 4, is_until);
    } else {
        fprintf(stderr, "acsh: syntax error: expected 'done'\n");
    }

    free(buf);
    return status;
                       }

                       /* Runs one complete for VAR in WORD...; do BODY; done loop spanning
                        * [start, end), where `start` points at the "for" keyword and `end`
                        * points just past the closing "done". Parses the header (variable
                        * name, then the word list up to "do"), $VAR/glob-expands each word
                        * in the list exactly the way an ordinary command argument would be
                        * (reusing env_expand/glob_expand from env.c/glob.c), then runs the
                        * body once per word with that variable set via setenv() -- so the
                        * body can refer to it as $VAR like any other variable. */
                       static int run_for_block(const char *start, const char *end) {
                           const char *cursor = start + 3; /* skip "for" */
                           while (cursor < end && (*cursor == ' ' || *cursor == '\t')) cursor++;

                           /* variable name: up to the next whitespace */
                           const char *name_start = cursor;
                           size_t name_len = 0;
                           while (cursor + name_len < end && cursor[name_len] != ' ' &&
                               cursor[name_len] != '\t' && cursor[name_len] != '\n') {
                               name_len++;
                               }
                               if (name_len == 0 || name_len >= 64) {
                                   fprintf(stderr, "acsh: syntax error: invalid 'for' variable name\n");
                                   return 1;
                               }
                               char varname[64];
                           memcpy(varname, name_start, name_len);
                           varname[name_len] = '\0';
                           cursor += name_len;

                           while (cursor < end && (*cursor == ' ' || *cursor == '\t' || *cursor == '\n')) cursor++;

                           /* expect "in" */
                           if (!(cursor + 2 <= end && word_matches(cursor, 2, "in") &&
                               (cursor + 2 == end || cursor[2] == ' ' || cursor[2] == '\t' ||
                               cursor[2] == '\n' || cursor[2] == ';'))) {
                               fprintf(stderr, "acsh: syntax error: expected 'in' after 'for %s'\n", varname);
                           return 1;
                               }
                               cursor += 2;

                               /* word list: everything up to "do", tokenized on whitespace (a
                                * simplified tokenizer here -- no quoting support within a for
                                * word list is a known scope limitation; the common case of bare
                                * words and $VAR references works correctly) */
                               const char *kw;
                               const char *do_pos = find_next_do_done(cursor, end, &kw);
                               if (do_pos == NULL || strcmp(kw, "do") != 0) {
                                   fprintf(stderr, "acsh: syntax error: expected 'do'\n");
                                   return 1;
                               }
                               const char *wordlist_start = cursor;
                               const char *wordlist_end = do_pos;
                               const char *body_start = do_pos + 2;

                               const char *done_kw;
                               const char *done_pos = find_next_do_done(body_start, end, &done_kw);
                               if (done_pos == NULL || strcmp(done_kw, "done") != 0) {
                                   fprintf(stderr, "acsh: syntax error: expected 'done'\n");
                                   return 1;
                               }
                               const char *body_end = done_pos;

                               /* tokenize the word list into raw words (whitespace-separated,
                                * quote-aware just enough to skip splitting inside quotes) */
                               #define FOR_MAX_WORDS 256
                               char *raw_words[FOR_MAX_WORDS];
                               int num_words = 0;
                               {
                                   const char *p = wordlist_start;
                                   while (p < wordlist_end) {
                                       while (p < wordlist_end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == ';')) p++;
                                       if (p >= wordlist_end) break;

                                       const char *word_start = p;
                                       char in_quote = '\0';
                                       int subst_depth = 0;   /* inside $( ... ), possibly nested */
                                       int in_backtick = 0;   /* inside `...` */
                                       while (p < wordlist_end) {
                                           if (in_quote != '\0') {
                                               if (*p == in_quote) in_quote = '\0';
                                               p++;
                                               continue;
                                           }
                                           if (*p == '\'' || *p == '"') { in_quote = *p; p++; continue; }

                                           /* a $(...) or `...` region is ONE indivisible part of
                                            * the word: spaces and ;'s inside it must not split it */
                                           if (*p == '`') { in_backtick = !in_backtick; p++; continue; }
                                           if (*p == '$' && p + 1 < wordlist_end && p[1] == '(') {
                                               subst_depth++;
                                               p += 2;
                                               continue;
                                           }
                                           if (subst_depth > 0) {
                                               if (*p == '(') subst_depth++;
                                               else if (*p == ')') subst_depth--;
                                               p++;
                                               continue;
                                           }
                                           if (in_backtick) { p++; continue; }

                                           if (*p == ' ' || *p == '\t' || *p == '\n' || *p == ';') break;
                                           p++;
                                       }

                                       if (num_words < FOR_MAX_WORDS) {
                                           size_t wl = (size_t)(p - word_start);
                                           char *w = malloc(wl + 1);
                                           memcpy(w, word_start, wl);
                                           w[wl] = '\0';
                                           raw_words[num_words++] = w;
                                       }
                                   }
                               }

                               int result_status = 0;
                               const char *saved_value = getenv(varname); /* to restore after the loop, POSIX-style */
                               char *saved_value_copy = (saved_value != NULL) ? strdup(saved_value) : NULL;

                               active_loop_depth++;

                               for (int i = 0; i < num_words; i++) {
                                   /* $VAR expansion first (env_expand also strips quote sentinels
                                    * via parser.c's convention -- but since we tokenized this
                                    * word list ourselves above rather than through read_word(),
                                    * there are no sentinel bytes to strip here; env_expand still
                                    * correctly expands any literal $VAR text in the word). */
                                   char *expanded = env_expand(raw_words[i]);
                                   const char *to_split = (expanded != NULL) ? expanded : raw_words[i];

                                   /* POSIX word-splitting: an expansion's result is re-split on
                                    * whitespace into potentially SEVERAL loop words -- e.g. `for
                                    * i in $LIST` where LIST="x y z" must iterate 3 times, not
                                    * treat "x y z" as one item. This is a deliberate
                                    * simplification vs full POSIX (which only splits unquoted
                                    * expansions and respects $IFS); acsh always splits on
                                    * space/tab here, which covers the common case. Glob
                                    * expansion is then applied to each split piece, so a pattern
                                    * like "$DIR" followed by "star.c" also works (glob happens
                                    * after both $VAR expansion and splitting, same order real
                                    * shells use). */
                                   const char *sp = to_split;
                                   while (*sp != '\0') {
                                       while (*sp == ' ' || *sp == '\t') sp++;
                                       if (*sp == '\0') break;
                                       const char *piece_start = sp;
                                       while (*sp != '\0' && *sp != ' ' && *sp != '\t') sp++;
                                       size_t piece_len = (size_t)(sp - piece_start);

                                       char piece[ACSH_MAX_LINE];
                                       if (piece_len >= sizeof(piece)) piece_len = sizeof(piece) - 1;
                                       memcpy(piece, piece_start, piece_len);
                                       piece[piece_len] = '\0';

                                       char *glob_results[64];
                                       int nglob = glob_expand(piece, glob_results, 64);
                                       for (int g = 0; g < nglob; g++) {
                                           setenv(varname, glob_results[g], 1);
                                           result_status = run_statements(body_start, body_end);
                                           free(glob_results[g]);

                                           /* Consume a pending break/continue if it targets THIS
                                            * loop (same level-decrement convention as
                                            * run_loop_block() above). */
                                           if (loop_signal != LOOP_SIGNAL_NONE) {
                                               if (loop_signal_level <= 1) {
                                                   LoopSignal sig = loop_signal;
                                                   loop_signal = LOOP_SIGNAL_NONE;
                                                   loop_signal_level = 0;
                                                   if (sig == LOOP_SIGNAL_BREAK) {
                                                       /* free any remaining un-freed glob results
                                                        * from this inner batch before leaving */
                                                       for (int g2 = g + 1; g2 < nglob; g2++) free(glob_results[g2]);
                                                       free(expanded);
                                                       goto for_loop_done;
                                                   }
                                                   /* CONTINUE: skip the rest of this glob batch
                                                    * and this split-piece batch, move to the
                                                    * next raw word */
                                                   for (int g2 = g + 1; g2 < nglob; g2++) free(glob_results[g2]);
                                                   goto next_raw_word;
                                               } else {
                                                   loop_signal_level--;
                                                   for (int g2 = g + 1; g2 < nglob; g2++) free(glob_results[g2]);
                                                   free(expanded);
                                                   goto for_loop_done; /* propagate up */
                                               }
                                           }
                                       }
                                   }

                                   next_raw_word:
                                   free(expanded);
                               }

                               for_loop_done:
                                   active_loop_depth--;

                               for (int i = 0; i < num_words; i++) {
                                   free(raw_words[i]);
                               }
                               #undef FOR_MAX_WORDS

                               /* restore the variable's previous value (or unset it if it had
                                * none before), matching how POSIX scopes the for-loop variable
                                * as an ordinary assignment rather than something block-local */
                               if (saved_value_copy != NULL) {
                                   setenv(varname, saved_value_copy, 1);
                                   free(saved_value_copy);
                               } else {
                                   unsetenv(varname);
                               }

                               return result_status;
                       }

                       /* Runs a full for VAR in WORD...; do BODY; done loop construct.
                        * Mirrors run_if_statement()/run_loop_statement()'s line-accumulation
                        * strategy, then hands off to run_for_block(). */
                       int run_for_statement(const char *first_line,
                                             char *(*read_more_line)(char *buf, int size)) {
                           size_t cap = ACSH_MAX_LINE * 4;
                           char *buf = malloc(cap);
                           if (buf == NULL) {
                               fprintf(stderr, "acsh: out of memory\n");
                               return 1;
                           }
                           size_t len = 0;

                           {
                               size_t plen = strlen(first_line);
                               while (len + plen + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                               memcpy(buf + len, first_line, plen);
                               len += plen;
                               buf[len++] = '\n';
                           }

                           while (!has_top_level_done(buf + 3, len - 3)) { /* +3 skips "for" */
                               char linebuf[ACSH_MAX_LINE];
                               if (read_more_line(linebuf, sizeof(linebuf)) == NULL) {
                                   fprintf(stderr, "acsh: syntax error: unexpected end of input, expected 'done'\n");
                                   free(buf);
                                   return 1;
                               }
                               size_t llen = strlen(linebuf);
                               while (len + llen + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                               memcpy(buf + len, linebuf, llen);
                               len += llen;
                               if (len == 0 || buf[len - 1] != '\n') {
                                   buf[len++] = '\n';
                               }
                           }

                           const char *kw;
                           const char *scan = buf + 3;
                           const char *done_at = NULL;
                           while (1) {
                               const char *found = find_next_do_done(scan, buf + len, &kw);
                               if (found == NULL) {
                                   break;
                               }
                               if (strcmp(kw, "done") == 0) {
                                   done_at = found;
                                   break;
                               }
                               scan = found + strlen(kw);
                           }

                           int status = 1;
                           if (done_at != NULL) {
                               status = run_for_block(buf, done_at + 4);
                           } else {
                               fprintf(stderr, "acsh: syntax error: expected 'done'\n");
                           }

                           free(buf);
                           return status;
                                             }

                                             /* Runs one complete case WORD in PATTERN1) BODY1 ;; ... esac block
                                              * spanning [start, end), where `start` points at the "case" keyword
                                              * and `end` points just past the closing "esac". Parses the subject
                                              * word (itself $VAR/glob-expanded like any command argument), then
                                              * walks each `PATTERN) BODY ;;` clause in turn, matching the subject
                                              * against each clause's pattern(s) with fnmatch() (POSIX glob-style
                                              * patterns: *, ?, [abc] -- the same metacharacters glob.c uses for
                                              * filenames, reused here for text matching instead). Multiple
                                              * patterns per clause separated by `|` are supported (e.g. "a|b)"),
                                              * matching standard case syntax. Runs only the FIRST matching
                                              * clause's body, then stops -- POSIX case has no fallthrough between
                                              * clauses (there is no C-style "break" needed). */
                                             static int run_case_block(const char *start, const char *end) {
                                                 const char *cursor = start + 4; /* skip "case" */
                                                 while (cursor < end && (*cursor == ' ' || *cursor == '\t')) cursor++;

                                                 /* subject word: up to whitespace */
                                                 const char *word_start = cursor;
                                                 size_t word_len = 0;
                                                 while (cursor + word_len < end && cursor[word_len] != ' ' &&
                                                     cursor[word_len] != '\t' && cursor[word_len] != '\n') {
                                                     word_len++;
                                                     }
                                                     if (word_len == 0) {
                                                         fprintf(stderr, "acsh: syntax error: expected a word after 'case'\n");
                                                         return 1;
                                                     }
                                                     char raw_subject[ACSH_MAX_LINE];
                                                 size_t copy_len = word_len < sizeof(raw_subject) - 1 ? word_len : sizeof(raw_subject) - 1;
                                                 memcpy(raw_subject, word_start, copy_len);
                                                 raw_subject[copy_len] = '\0';
                                                 cursor += word_len;

                                                 char *expanded_subject = env_expand(raw_subject);
                                                 const char *subject = (expanded_subject != NULL) ? expanded_subject : raw_subject;

                                                 while (cursor < end && (*cursor == ' ' || *cursor == '\t' || *cursor == '\n')) cursor++;

                                                 /* expect "in" */
                                                 if (!(cursor + 2 <= end && word_matches(cursor, 2, "in") &&
                                                     (cursor + 2 == end || cursor[2] == ' ' || cursor[2] == '\t' ||
                                                     cursor[2] == '\n' || cursor[2] == ';'))) {
                                                     fprintf(stderr, "acsh: syntax error: expected 'in' after 'case %s'\n", raw_subject);
                                                 free(expanded_subject);
                                                 return 1;
                                                     }
                                                     cursor += 2;

                                                     int result_status = 0;
                                                     int matched_and_ran = 0;

                                                     /* walk clauses: PATTERN[|PATTERN...]) BODY ;; (or the last clause
                                                      * may omit the trailing ;; right before esac, which POSIX allows) */
                                                     while (cursor < end) {
                                                         while (cursor < end && (*cursor == ' ' || *cursor == '\t' ||
                                                             *cursor == '\n' || *cursor == ';')) cursor++;
                                                         if (cursor >= end) break;

                                                         /* stray "esac" reached with nothing left to match -- normal
                                                          * termination of the walk, not an error */
                                                         if (cursor + 4 <= end && word_matches(cursor, 4, "esac")) {
                                                             break;
                                                         }

                                                         /* an optional leading '(' before the first pattern is allowed
                                                          * by POSIX (e.g. "(a) body ;;") -- skip it if present */
                                                         if (*cursor == '(') cursor++;

                                                         /* read patterns up to the ')' that closes this clause's
                                                          * pattern list, tracking quotes so a ')' inside a quoted
                                                          * pattern doesn't end the list early */
                                                         const char *patterns_start = cursor;
                                                         char in_quote = '\0';
                                                         while (cursor < end) {
                                                             if (in_quote != '\0') {
                                                                 if (*cursor == in_quote) in_quote = '\0';
                                                                 cursor++;
                                                                 continue;
                                                             }
                                                             if (*cursor == '\'' || *cursor == '"') { in_quote = *cursor; cursor++; continue; }
                                                             if (*cursor == ')') break;
                                                             cursor++;
                                                         }
                                                         if (cursor >= end) {
                                                             fprintf(stderr, "acsh: syntax error: expected ')' in case pattern\n");
                                                             free(expanded_subject);
                                                             return 1;
                                                         }
                                                         const char *patterns_end = cursor;
                                                         cursor++; /* skip ')' */

                                                         /* find this clause's body end: the next ";;" OR "esac",
                                                          * whichever comes first, at THIS case block's own nesting
                                                          * level (a nested case/if/while/for inside the body is
                                                          * skipped opaquely via the same helpers used elsewhere) */
                                                         const char *body_start = cursor;
                                                         const char *body_end = NULL;
                                                         {
                                                             const char *q = cursor;
                                                             char bq = '\0';
                                                             while (q < end) {
                                                                 if (bq != '\0') {
                                                                     if (*q == bq) bq = '\0';
                                                                     q++;
                                                                     continue;
                                                                 }
                                                                 if (*q == '\'' || *q == '"') { bq = *q; q++; continue; }

                                                                 if (*q == 'c' && q + 4 <= end && word_matches(q, 4, "case") &&
                                                                     (q == body_start || q[-1] == ' ' || q[-1] == '\t' ||
                                                                     q[-1] == '\n' || q[-1] == ';')) {
                                                                     const char *after = skip_case_block(q, end);
                                                                 if (after == NULL) {
                                                                     fprintf(stderr, "acsh: syntax error: unterminated nested 'case'\n");
                                                                     free(expanded_subject);
                                                                     return 1;
                                                                 }
                                                                 q = after;
                                                                 continue;
                                                                     }

                                                                     if (*q == ';' && q + 1 < end && q[1] == ';') {
                                                                         body_end = q;
                                                                         break;
                                                                     }
                                                                     if (q + 4 <= end && word_matches(q, 4, "esac") &&
                                                                         (q == body_start || q[-1] == ' ' || q[-1] == '\t' ||
                                                                         q[-1] == '\n' || q[-1] == ';')) {
                                                                         body_end = q;
                                                                     break;
                                                                         }
                                                                         q++;
                                                             }
                                                         }
                                                         if (body_end == NULL) {
                                                             body_end = end; /* malformed, but be lenient: run to end */
                                                         }

                                                         /* does the subject match ANY of this clause's |-separated
                                                          * patterns? Each pattern is itself $VAR-expanded before
                                                          * matching, same as the case subject. */
                                                         int this_clause_matches = 0;
                                                         {
                                                             const char *pp = patterns_start;
                                                             while (pp < patterns_end) {
                                                                 const char *pat_start = pp;
                                                                 char pq = '\0';
                                                                 while (pp < patterns_end) {
                                                                     if (pq != '\0') {
                                                                         if (*pp == pq) pq = '\0';
                                                                         pp++;
                                                                         continue;
                                                                     }
                                                                     if (*pp == '\'' || *pp == '"') { pq = *pp; pp++; continue; }
                                                                     if (*pp == '|') break;
                                                                     pp++;
                                                                 }
                                                                 size_t pat_len = (size_t)(pp - pat_start);

                                                                 char pattern_buf[256];
                                                                 size_t pl = pat_len < sizeof(pattern_buf) - 1 ? pat_len : sizeof(pattern_buf) - 1;
                                                                 /* trim surrounding whitespace from the pattern */
                                                                 while (pl > 0 && (pat_start[0] == ' ' || pat_start[0] == '\t')) { pat_start++; pl--; pat_len--; }
                                                                 while (pl > 0 && (pat_start[pl - 1] == ' ' || pat_start[pl - 1] == '\t')) pl--;
                                                                 memcpy(pattern_buf, pat_start, pl);
                                                                 pattern_buf[pl] = '\0';

                                                                 char *expanded_pattern = env_expand(pattern_buf);
                                                                 const char *final_pattern = (expanded_pattern != NULL) ? expanded_pattern : pattern_buf;

                                                                 if (fnmatch(final_pattern, subject, 0) == 0) {
                                                                     this_clause_matches = 1;
                                                                 }
                                                                 free(expanded_pattern);

                                                                 if (pp < patterns_end) pp++; /* skip '|' */
                                                                     if (this_clause_matches) break;
                                                             }
                                                         }

                                                         if (this_clause_matches && !matched_and_ran) {
                                                             result_status = run_statements(body_start, body_end);
                                                             matched_and_ran = 1;
                                                         }

                                                         cursor = body_end;
                                                         if (cursor + 1 < end && cursor[0] == ';' && cursor[1] == ';') {
                                                             cursor += 2;
                                                         }
                                                         /* if body_end landed exactly on "esac" (no trailing ;;), the
                                                          * outer while's own boundary check will stop the loop next
                                                          * iteration since cursor now points at "esac" */
                                                     }

                                                     free(expanded_subject);
                                                     return result_status;
                                             }

                                             /* Runs a full case WORD in ... esac construct. Mirrors the other
                                              * run_*_statement() functions' line-accumulation strategy, then hands
                                              * off to run_case_block(). */
                                             int run_case_statement(const char *first_line,
                                                                    char *(*read_more_line)(char *buf, int size)) {
                                                 size_t cap = ACSH_MAX_LINE * 4;
                                                 char *buf = malloc(cap);
                                                 if (buf == NULL) {
                                                     fprintf(stderr, "acsh: out of memory\n");
                                                     return 1;
                                                 }
                                                 size_t len = 0;

                                                 {
                                                     size_t plen = strlen(first_line);
                                                     while (len + plen + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                                                     memcpy(buf + len, first_line, plen);
                                                     len += plen;
                                                     buf[len++] = '\n';
                                                 }

                                                 while (skip_case_block(buf, buf + len) == NULL) {
                                                     char linebuf[ACSH_MAX_LINE];
                                                     if (read_more_line(linebuf, sizeof(linebuf)) == NULL) {
                                                         fprintf(stderr, "acsh: syntax error: unexpected end of input, expected 'esac'\n");
                                                         free(buf);
                                                         return 1;
                                                     }
                                                     size_t llen = strlen(linebuf);
                                                     while (len + llen + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                                                     memcpy(buf + len, linebuf, llen);
                                                     len += llen;
                                                     if (len == 0 || buf[len - 1] != '\n') {
                                                         buf[len++] = '\n';
                                                     }
                                                 }

                                                 const char *after_esac = skip_case_block(buf, buf + len);
                                                 int status = run_case_block(buf, after_esac);

                                                 free(buf);
                                                 return status;
                                                                    }

                                                                    int run_if_statement(const char *first_line,
                                                                                         char *(*read_more_line)(char *buf, int size)) {
                                                                        /* Accumulate lines into one growable buffer until a top-level
                                                                         * "fi" exists -- this is what allows both single-line
                                                                         * (`if x; then y; fi`) and multi-line script-style if blocks
                                                                         * (including ones containing further nested if/fi blocks). */
                                                                        size_t cap = ACSH_MAX_LINE * 4;
                                                                        char *buf = malloc(cap);
                                                                        if (buf == NULL) {
                                                                            fprintf(stderr, "acsh: out of memory\n");
                                                                            return 1;
                                                                        }
                                                                        size_t len = 0;

                                                                        {
                                                                            size_t plen = strlen(first_line);
                                                                            while (len + plen + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                                                                            memcpy(buf + len, first_line, plen);
                                                                            len += plen;
                                                                            buf[len++] = '\n';
                                                                        }

                                                                        while (!has_top_level_fi(buf + 2, len - 2)) {
                                                                            char linebuf[ACSH_MAX_LINE];
                                                                            if (read_more_line(linebuf, sizeof(linebuf)) == NULL) {
                                                                                fprintf(stderr, "acsh: syntax error: unexpected end of input, expected 'fi'\n");
                                                                                free(buf);
                                                                                return 1;
                                                                            }
                                                                            size_t llen = strlen(linebuf);
                                                                            while (len + llen + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                                                                            memcpy(buf + len, linebuf, llen);
                                                                            len += llen;
                                                                            if (len == 0 || buf[len - 1] != '\n') {
                                                                                buf[len++] = '\n';
                                                                            }
                                                                        }

                                                                        /* buf now starts with "if" (from first_line) and contains a
                                                                         * top-level "fi" somewhere within [buf, buf+len) -- find exactly
                                                                         * where that "fi" ends so run_if_block() gets a precise end
                                                                         * bound rather than the whole (possibly longer) buffer. */
                                                                        const char *kw;
                                                                        const char *scan = buf + 2;
                                                                        const char *fi_at = NULL;
                                                                        while (1) {
                                                                            const char *found = find_next_keyword(scan, buf + len, &kw);
                                                                            if (found == NULL) {
                                                                                break; /* shouldn't happen, has_top_level_fi already confirmed one exists */
                                                                            }
                                                                            if (strcmp(kw, "fi") == 0) {
                                                                                fi_at = found;
                                                                                break;
                                                                            }
                                                                            scan = found + strlen(kw);
                                                                        }

                                                                        int status = 1;
                                                                        if (fi_at != NULL) {
                                                                            status = run_if_block(buf, fi_at + 2);
                                                                        } else {
                                                                            fprintf(stderr, "acsh: syntax error: expected 'fi'\n");
                                                                        }

                                                                        free(buf);
                                                                        return status;
                                                                                         }

                                                                                         /* Public entry point onto the internal run_statements() engine above,
                                                                                          * for functions.c to run a called function's body through the exact
                                                                                          * same "sequence of statements/nested constructs" machinery that
                                                                                          * if/while/for/case bodies already use here. Kept as a thin wrapper
                                                                                          * (rather than making run_statements itself non-static) so this
                                                                                          * file's internal helpers stay clearly private to anyone reading
                                                                                          * acsh.h, with just this one function as the deliberate exception. */
                                                                                         int run_statements_entrypoint(const char *start, const char *end) {
                                                                                             return run_statements(start, end);
                                                                                         }
