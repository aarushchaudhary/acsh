#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "acsh.h"

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

                if (wlen > 0 && (word_matches(p, wlen, "while") ||
                    word_matches(p, wlen, "until") ||
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
            if (wlen > 0 && (word_matches(p, wlen, "while") ||
                word_matches(p, wlen, "until") ||
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
    }

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
