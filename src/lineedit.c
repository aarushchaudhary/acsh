#define _DEFAULT_SOURCE           /* TIOCGWINSZ, cfmakeraw-free termios bits */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include "acsh.h"

/* The interactive line editor (emacs keys, as in bash and ash).
 *
 *   Left/Right Ctrl-B/F      move a character      Home/End Ctrl-A/E   line start/end
 *   Alt-B/F Ctrl-Left/Right  move a word           Backspace Delete    delete a character
 *   Ctrl-U / Ctrl-K          cut to start / end    Ctrl-W              cut the word before
 *   Ctrl-Y                   paste the last cut    Ctrl-L              clear the screen
 *   Up/Down Ctrl-P/N         history               Ctrl-R              search history backwards
 *   Tab                      complete              Ctrl-C              cancel the line
 *   Ctrl-D                   delete, or end input on an empty line (not under set -o ignoreeof)
 *
 * The terminal is in raw mode only while a line is typed. A line wider
 * than the screen scrolls sideways, so redrawing never depends on how a
 * terminal wraps. Without a terminal (or with TERM=dumb) the shell
 * reads plain lines instead and this file is not used. */

typedef struct {
    char  *buf;
    size_t len, cap;
    size_t pos;              /* cursor, a byte offset at a character boundary */
    const char *prompt;      /* the prompt's last line, no-width markers kept */
    size_t prompt_cols;
    char  *suggestion;       /* history suggestion shown after the line (3.9), or NULL */
} Line;

static struct termios saved_tty;
static int raw_on = 0;
static char *kill_buffer = NULL;

static void out(const char *s, size_t n) {
    while (n > 0) {
        ssize_t w = write(STDERR_FILENO, s, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

static void outs(const char *s) {
    out(s, strlen(s));
}

static int raw_mode(void) {
    if (raw_on) return 0;
    if (tcgetattr(STDIN_FILENO, &saved_tty) != 0) return -1;
    struct termios raw = saved_tty;
    raw.c_iflag &= ~(unsigned)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_lflag &= ~(unsigned)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return -1;
    raw_on = 1;
    return 0;
}

static void cooked_mode(void) {
    if (raw_on) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_tty);
        raw_on = 0;
    }
}

int lineedit_usable(void) {
    const char *term = var_get("TERM");
    return isatty(STDIN_FILENO) && isatty(STDERR_FILENO) &&
           !(term != NULL && (strcmp(term, "dumb") == 0 || strcmp(term, "cons25") == 0 || strcmp(term, "emacs") == 0));
}

static size_t screen_cols(void) {
    struct winsize ws;
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    const char *c = var_get("COLUMNS");
    long n = c ? strtol(c, NULL, 10) : 0;
    return n > 10 ? (size_t)n : 80;
}

/* Bytes of the UTF-8 character starting at s[i] / ending before s[i]. */
static size_t next_char(const char *s, size_t len, size_t i) {
    if (i >= len) return i;
    i++;
    while (i < len && ((unsigned char)s[i] & 0xc0) == 0x80) i++;
    return i;
}

static size_t prev_char(const char *s, size_t i) {
    if (i == 0) return 0;
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xc0) == 0x80) i--;
    return i;
}

/* Columns a stretch of the line takes: control characters show as ^X. */
static size_t cols_of(const char *s, size_t from, size_t to) {
    size_t c = 0;
    for (size_t i = from; i < to; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x20 || ch == 0x7f) c += 2;
        else if ((ch & 0xc0) != 0x80) c++;
    }
    return c;
}

static void put_visible(Line *l, size_t from, size_t to, char *o, size_t *k) {
    for (size_t i = from; i < to; i++) {
        unsigned char ch = (unsigned char)l->buf[i];
        if (ch < 0x20 || ch == 0x7f) {
            o[(*k)++] = '^';
            o[(*k)++] = (char)(ch == 0x7f ? '?' : ch + '@');
        } else {
            o[(*k)++] = (char)ch;
        }
    }
}

/* A prompt wider than half the screen is shown as "<" and its last
 * part (plain text: colour codes dropped), so the line keeps room. */
static char *short_prompt(const char *prompt, size_t keep) {
    size_t n = strlen(prompt);
    char *plain = malloc(n + 2);
    size_t k = 0;
    int hidden = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)prompt[i];
        if (c == 0x1c) { hidden = 1; continue; }
        if (c == 0x1d) { hidden = 0; continue; }
        if (hidden) continue;
        if (c == 0x1b && prompt[i + 1] == '[') {
            for (i += 2; i < n && !(prompt[i] >= 0x40 && prompt[i] <= 0x7e); i++) {}
            continue;
        }
        plain[k++] = (char)c;
    }
    plain[k] = '\0';
    size_t start = k, cols = 0;
    while (start > 0 && cols < keep) {
        start--;
        if (((unsigned char)plain[start] & 0xc0) != 0x80) cols++;
    }
    char *out = malloc(k - start + 2);
    out[0] = '<';
    strcpy(out + 1, plain + start);
    free(plain);
    return out;
}

/* Redraws the prompt's last line and the visible part of the text. */
static void refresh(Line *l) {
    size_t cols = screen_cols();
    const char *prompt = l->prompt;
    size_t prompt_cols = l->prompt_cols;
    char *shortened = NULL;
    if (prompt_cols > cols / 2) {
        shortened = short_prompt(l->prompt, cols / 2 - 1);
        prompt = shortened;
        prompt_cols = cols / 2;
    }
    size_t avail = cols > prompt_cols + 2 ? cols - prompt_cols - 1 : 1;
    /* scroll so the cursor is visible */
    size_t start = 0;
    while (cols_of(l->buf, start, l->pos) >= avail) start = next_char(l->buf, l->len, start);
    size_t end = start;
    while (end < l->len && cols_of(l->buf, start, next_char(l->buf, l->len, end)) < avail) {
        end = next_char(l->buf, l->len, end);
    }
    size_t need = strlen(prompt) + (end - start) * 2 + 64 + (l->suggestion ? strlen(l->suggestion) * 2 : 0);
    char *o = malloc(need);
    size_t k = 0;
    o[k++] = '\r';
    for (const char *p = prompt; *p; p++) {
        if (*p != '\x1c' && *p != '\x1d') o[k++] = *p;
    }
    put_visible(l, start, end, o, &k);
    size_t shown = cols_of(l->buf, start, end);
    if (l->suggestion != NULL && end == l->len && l->pos == l->len) {
        /* the rest of a history match, in grey, if it fits */
        size_t room = avail > shown ? avail - shown : 0;
        size_t n = strlen(l->suggestion);
        if (n > room) n = room;
        if (n > 0) {
            memcpy(o + k, "\033[90m", 5); k += 5;
            for (size_t i = 0; i < n; i++) {
                unsigned char ch = (unsigned char)l->suggestion[i];
                o[k++] = (ch < 0x20 || ch == 0x7f) ? ' ' : (char)ch;
            }
            memcpy(o + k, "\033[0m", 4); k += 4;
        }
    }
    memcpy(o + k, "\033[K", 3); k += 3;   /* clear the rest of the screen line */
    /* the cursor: back to the start of the line, then right to its column */
    size_t col = prompt_cols + cols_of(l->buf, start, l->pos);
    o[k++] = '\r';
    if (col > 0) k += (size_t)snprintf(o + k, need - k, "\033[%zuC", col);
    out(o, k);
    free(o);
    free(shortened);
}

static void make_room(Line *l, size_t extra) {
    if (l->len + extra + 2 > l->cap) {
        l->cap = (l->len + extra + 2) * 2;
        l->buf = realloc(l->buf, l->cap);
    }
}

static void insert_text(Line *l, const char *s, size_t n) {
    make_room(l, n);
    memmove(l->buf + l->pos + n, l->buf + l->pos, l->len - l->pos + 1);
    memcpy(l->buf + l->pos, s, n);
    l->len += n;
    l->pos += n;
}

static void delete_range(Line *l, size_t from, size_t to, int save) {
    if (to <= from) return;
    if (save) {
        free(kill_buffer);
        kill_buffer = strndup(l->buf + from, to - from);
    }
    memmove(l->buf + from, l->buf + to, l->len - to + 1);
    l->len -= to - from;
    if (l->pos > to) l->pos -= to - from;
    else if (l->pos > from) l->pos = from;
}

static void set_text(Line *l, const char *s) {
    make_room(l, 0);
    l->len = 0;
    l->pos = 0;
    l->buf[0] = '\0';      /* keep the text NUL-terminated at len */
    insert_text(l, s, strlen(s));
}

static int is_word(char c) {
    return c != ' ' && c != '\t' && c != '/' && c != '\n' && c != '=' && c != ':';
}

static size_t word_left(Line *l) {
    size_t i = l->pos;
    while (i > 0 && !is_word(l->buf[i - 1])) i--;
    while (i > 0 && is_word(l->buf[i - 1])) i--;
    return i;
}

static size_t word_right(Line *l) {
    size_t i = l->pos;
    while (i < l->len && !is_word(l->buf[i])) i++;
    while (i < l->len && is_word(l->buf[i])) i++;
    return i;
}

static int read_key(void) {
    unsigned char c;
    for (;;) {
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n == 1) return c;
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
}

enum { K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_DEL, K_WLEFT, K_WRIGHT, K_IGNORE };

/* After ESC: the rest of an escape sequence, as one key. */
static int read_escape(void) {
    int a = read_key();
    if (a == 'b' || a == 'B') return K_WLEFT;     /* Alt-B */
    if (a == 'f' || a == 'F') return K_WRIGHT;    /* Alt-F */
    if (a == 0x7f) return 0x17;                   /* Alt-Backspace: as Ctrl-W */
    if (a != '[' && a != 'O') return K_IGNORE;
    int b = read_key();
    if (a == 'O') {
        switch (b) {
        case 'A': return K_UP;    case 'B': return K_DOWN;
        case 'C': return K_RIGHT; case 'D': return K_LEFT;
        case 'H': return K_HOME;  case 'F': return K_END;
        }
        return K_IGNORE;
    }
    if (b >= '0' && b <= '9') {
        int num = b - '0', c;
        int mod = 0;
        while ((c = read_key()) >= '0' && c <= '9') num = num * 10 + (c - '0');
        if (c == ';') {                         /* ESC [ 1 ; 5 C: with a modifier */
            while ((c = read_key()) >= '0' && c <= '9') mod = mod * 10 + (c - '0');
            if (c == 'C') return mod >= 3 ? K_WRIGHT : K_RIGHT;
            if (c == 'D') return mod >= 3 ? K_WLEFT : K_LEFT;
            if (c == 'H') return K_HOME;
            if (c == 'F') return K_END;
            return K_IGNORE;
        }
        if (c != '~') return K_IGNORE;
        switch (num) {
        case 1: case 7: return K_HOME;
        case 4: case 8: return K_END;
        case 3: return K_DEL;
        }
        return K_IGNORE;
    }
    switch (b) {
    case 'A': return K_UP;    case 'B': return K_DOWN;
    case 'C': return K_RIGHT; case 'D': return K_LEFT;
    case 'H': return K_HOME;  case 'F': return K_END;
    }
    return K_IGNORE;
}

/* ---- history suggestion (3.9) ---- */

static int suggestions_on(void) {
    return shell_option('S');
}

static void update_suggestion(Line *l) {
    free(l->suggestion);
    l->suggestion = NULL;
    if (!suggestions_on() || l->len == 0 || l->pos != l->len) return;
    for (int i = history_length() - 1; i >= 0; i--) {
        const char *h = history_get(i);
        if (strncmp(h, l->buf, l->len) == 0 && h[l->len] != '\0' && strchr(h, '\n') == NULL) {
            l->suggestion = strdup(h + l->len);
            return;
        }
    }
}

/* ---- Ctrl-R ---- */

/* Reverse incremental search. Returns the key that ended it (Enter runs
 * the match, other keys edit it), or -2 when cancelled (Ctrl-G/Ctrl-C). */
static int search_history(Line *l) {
    char query[256] = "";
    size_t qn = 0;
    int at = history_length();      /* searching below this index */
    int found = -1;
    char *orig = strndup(l->buf, l->len);
    for (;;) {
        char head[300];
        snprintf(head, sizeof(head), "(reverse-i-search)`%s': ", query);
        Line view = { NULL, 0, 0, 0, head, strlen(head), NULL };
        const char *match = found >= 0 ? history_get(found) : "";
        set_text(&view, match);
        view.pos = 0;
        if (found >= 0 && qn > 0) {
            const char *hit = strstr(match, query);
            view.pos = hit ? (size_t)(hit - match) : 0;
        }
        refresh(&view);
        free(view.buf);
        int c = read_key();
        if (c == 0x12) {                  /* Ctrl-R again: an older match */
            for (int i = (found >= 0 ? found : at) - 1; i >= 0; i--) {
                if (qn > 0 && strstr(history_get(i), query)) { found = i; break; }
            }
            continue;
        }
        if (c == 0x7f || c == 0x08) {     /* Backspace: shorter query, search again from the newest */
            if (qn > 0) query[--qn] = '\0';
            found = -1;
            for (int i = at - 1; i >= 0 && qn > 0; i--) {
                if (strstr(history_get(i), query)) { found = i; break; }
            }
            continue;
        }
        if (c >= 0x20 && c != 0x7f && qn + 1 < sizeof(query)) {
            query[qn++] = (char)c;
            query[qn] = '\0';
            int from = found >= 0 ? found + 1 : at;   /* the current match may still fit */
            found = -1;
            for (int i = (from > at ? at : from) - 1; i >= 0; i--) {
                if (strstr(history_get(i), query)) { found = i; break; }
            }
            continue;
        }
        if (c == 0x07 || c == 0x03 || c < 0) {        /* Ctrl-G, Ctrl-C: back to the line as it was */
            set_text(l, orig);
            free(orig);
            return -2;
        }
        set_text(l, found >= 0 ? history_get(found) : orig);
        free(orig);
        return c;   /* Enter, Esc, arrows...: the caller acts on it */
    }
}

/* ---- the editor ---- */

char *lineedit_read(const char *prompt) {
    const char *last = strrchr(prompt, '\n');
    if (last != NULL) {
        out(prompt, (size_t)(last - prompt + 1));   /* earlier prompt lines, once */
        prompt = last + 1;
    }
    Line l = { NULL, 0, 0, 0, prompt, prompt_width(prompt), NULL };
    make_room(&l, 64);
    l.buf[0] = '\0';
    if (raw_mode() != 0) {
        return NULL;
    }
    int hist = history_length();     /* Up/Down position; == length: the line being typed */
    char *typed = NULL;              /* that line, kept while browsing history */
    char *result = NULL;
    int done = 0;
    refresh(&l);
    while (!done) {
        int c = read_key();
        if (c == 0x1b) c = read_escape();
        if (c == 0x12) c = search_history(&l);
        switch (c) {
        case -1:                       /* input closed */
            if (l.len == 0) { done = -1; break; }
            /* fall through */
        case '\r': case '\n':
            l.pos = l.len;
            free(l.suggestion);
            l.suggestion = NULL;
            refresh(&l);
            done = 1;
            break;
        case -2:
            break;
        case 0x03:                     /* Ctrl-C: cancel the line */
            outs("^C");
            l.len = 0;
            l.buf[0] = '\0';
            done = 2;
            break;
        case 0x04:                     /* Ctrl-D */
            if (l.len > 0) {
                delete_range(&l, l.pos, next_char(l.buf, l.len, l.pos), 0);
            } else if (shell_option('I')) {
                outs("\r\nUse \"exit\" to leave the shell.\r\n");
            } else {
                done = -1;
            }
            break;
        case 0x01: case K_HOME: l.pos = 0; break;
        case 0x05: case K_END:  l.pos = l.len; break;
        case 0x02: case K_LEFT: l.pos = prev_char(l.buf, l.pos); break;
        case 0x06: case K_RIGHT:
            if (l.pos == l.len && l.suggestion != NULL) {   /* accept the suggestion */
                char *s = strdup(l.suggestion);
                insert_text(&l, s, strlen(s));
                free(s);
            } else {
                l.pos = next_char(l.buf, l.len, l.pos);
            }
            break;
        case K_WLEFT:  l.pos = word_left(&l); break;
        case K_WRIGHT: l.pos = word_right(&l); break;
        case 0x7f: case 0x08:          /* Backspace */
            delete_range(&l, prev_char(l.buf, l.pos), l.pos, 0);
            break;
        case K_DEL:
            delete_range(&l, l.pos, next_char(l.buf, l.len, l.pos), 0);
            break;
        case 0x15: delete_range(&l, 0, l.pos, 1); break;            /* Ctrl-U */
        case 0x0b: delete_range(&l, l.pos, l.len, 1); break;        /* Ctrl-K */
        case 0x17: delete_range(&l, word_left(&l), l.pos, 1); break; /* Ctrl-W */
        case 0x19:                                                   /* Ctrl-Y */
            if (kill_buffer) insert_text(&l, kill_buffer, strlen(kill_buffer));
            break;
        case 0x0c: outs("\033[H\033[2J"); break;                     /* Ctrl-L */
        case 0x10: case K_UP:
        case 0x0e: case K_DOWN: {
            int up = (c == 0x10 || c == K_UP);
            int to = up ? hist - 1 : hist + 1;
            if (to < 0 || to > history_length()) break;
            if (hist == history_length()) {
                free(typed);
                typed = strndup(l.buf, l.len);
            }
            hist = to;
            set_text(&l, hist == history_length() ? (typed ? typed : "") : history_get(hist));
            break;
        }
        case '\t': {
            size_t at = l.pos;
            char *with = lineedit_complete(l.buf, l.pos, &at);
            if (with != NULL) {
                set_text(&l, with);
                l.pos = at;   /* the cursor goes after the completed word */
                free(with);
            }
            break;
        }
        case K_IGNORE: case 0x07: case 0x1a: case 0x1c:
            break;
        default:
            if (c >= 0x20 && c < 0x100) {
                char ch = (char)c;
                insert_text(&l, &ch, 1);
            }
        }
        if (!done) {
            update_suggestion(&l);
            refresh(&l);
        }
    }
    cooked_mode();
    outs("\r\n");
    if (done == 1) {
        result = malloc(l.len + 2);
        memcpy(result, l.buf, l.len);
        result[l.len] = '\n';
        result[l.len + 1] = '\0';
    } else if (done == 2) {
        result = strdup("");           /* "" = cancelled (see stdin_lines) */
    }
    free(typed);
    free(l.suggestion);
    free(l.buf);
    return result;                     /* NULL: end of input */
}
