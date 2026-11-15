/* Interactive tests: acsh runs on a pseudo-terminal, as it does for a
 * person, and the tests type keys and read the screen output.
 *
 *     make check                       (builds this and runs every suite)
 *     tests/pty/pty_test [--minimal] [path/to/acsh]
 *
 * --minimal is for a `make MINIMAL=1` build: the line editor, completion
 * and suggestion tests are skipped, and plain line reading is checked.
 *
 * Tests that need root (writing /etc/acshrc, `su` to another user) run
 * only as root, as in the Alpine CI container, and are reported SKIP
 * otherwise.
 *
 * Plain POSIX (posix_openpt), so it builds the same on glibc and musl
 * with no extra libraries. */
#define _DEFAULT_SOURCE           /* TIOCSCTTY, memmem, mkdtemp */
#define _XOPEN_SOURCE 700         /* posix_openpt, grantpt, ptsname */
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static char acsh[4096];
static int minimal = 0;
static int npass = 0, nfail = 0, nskip = 0;
static char tmp[4096];

/* ---- a session: acsh (or another program) on a pty ---- */

typedef struct {
    pid_t  pid;
    int    fd;
    char  *out;      /* everything read from the terminal */
    size_t len, cap;
} Session;

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void append(Session *s, const char *data, size_t n) {
    if (s->len + n + 1 > s->cap) {
        s->cap = (s->len + n + 1) * 2;
        s->out = realloc(s->out, s->cap);
    }
    memcpy(s->out + s->len, data, n);
    s->len += n;
    s->out[s->len] = '\0';
}

/* Reads what the program writes for `wait` seconds (longer while it
 * keeps writing). */
static void sread(Session *s, double wait) {
    double end = now() + wait;
    while (now() < end) {
        struct pollfd p = { s->fd, POLLIN, 0 };
        if (poll(&p, 1, 50) <= 0) continue;
        char buf[65536];
        ssize_t n = read(s->fd, buf, sizeof(buf));
        if (n <= 0) break;            /* EIO: the program has exited */
        append(s, buf, (size_t)n);
        if (end < now() + 0.1) end = now() + 0.1;
    }
}

static void ssend(Session *s, const char *keys, double wait) {
    size_t n = strlen(keys);
    while (n > 0) {
        ssize_t w = write(s->fd, keys, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            break;
        }
        keys += w;
        n -= (size_t)w;
    }
    sread(s, wait);
}

static int contains(Session *s, const char *text) {
    return s->len > 0 && memmem(s->out, s->len, text, strlen(text)) != NULL;
}

static int expect(Session *s, const char *text, double timeout) {
    double end = now() + timeout;
    while (now() < end) {
        if (contains(s, text)) return 1;
        sread(s, 0.1);
    }
    return contains(s, text);
}

/* Starts argv (NULL: acsh) with exactly env, in cwd (NULL: here). */
static Session *start(char *const argv[], char *const env[], const char *cwd) {
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
        perror("pty_test: posix_openpt");
        exit(2);
    }
    const char *slave_name = ptsname(master);
    struct winsize ws = { 24, 80, 0, 0 };
    ioctl(master, TIOCSWINSZ, &ws);
    pid_t pid = fork();
    if (pid < 0) {
        perror("pty_test: fork");
        exit(2);
    }
    if (pid == 0) {
        setsid();
        int slave = open(slave_name, O_RDWR);
        if (slave < 0) _exit(127);
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2) close(slave);
        close(master);
        if (cwd != NULL && chdir(cwd) != 0) _exit(127);
        char *self[] = { acsh, NULL };
        execve(argv ? argv[0] : acsh, argv ? argv : self, env);
        _exit(127);
    }
    Session *s = calloc(1, sizeof(Session));
    s->pid = pid;
    s->fd = master;
    sread(s, 0.4);
    return s;
}

static void finish(Session *s) {
    ssize_t ignored = write(s->fd, "\nexit\n", 6);
    (void)ignored;
    struct timespec pause = { 0, 200000000 };
    nanosleep(&pause, NULL);
    kill(s->pid, SIGKILL);
    waitpid(s->pid, NULL, 0);
    close(s->fd);
    free(s->out);
    free(s);
}

/* ---- results ---- */

static void detail(Session *s, size_t last) {
    if (s == NULL) return;
    size_t from = s->len > last ? s->len - last : 0;
    fputs("    ", stdout);
    for (size_t i = from; i < s->len; i++) {
        unsigned char c = (unsigned char)s->out[i];
        if (c == '\\') fputs("\\\\", stdout);
        else if (c >= 0x20 && c < 0x7f) putchar(c);
        else printf("\\x%02x", c);
    }
    putchar('\n');
}

static void check(const char *name, int ok, Session *s) {
    if (ok) npass++;
    else nfail++;
    printf("%s%s\n", ok ? "PASS: " : "FAIL: ", name);
    if (!ok) detail(s, 400);
    fflush(stdout);
}

static void skip(const char *name, const char *why) {
    nskip++;
    printf("SKIP: %s (%s)\n", name, why);
}

/* ---- helpers ---- */

/* The environment `docker exec -it` or `lxc exec` gives: no ENV, little
 * else. extra: up to two more NAME=value entries (or NULL). */
static char **clean_env(const char *home, const char *extra1, const char *extra2) {
    static char *env[8];
    static char home_var[4300], term_var[64];
    snprintf(home_var, sizeof(home_var), "HOME=%s", home);
    int n = 0;
    env[n++] = home_var;
    env[n++] = "PATH=/usr/sbin:/usr/bin:/sbin:/bin";
    int term_given = (extra1 && strncmp(extra1, "TERM=", 5) == 0) || (extra2 && strncmp(extra2, "TERM=", 5) == 0);
    if (!term_given) {
        snprintf(term_var, sizeof(term_var), "TERM=xterm");
        env[n++] = term_var;
    }
    if (extra1) env[n++] = (char *)extra1;
    if (extra2) env[n++] = (char *)extra2;
    env[n] = NULL;
    return env;
}

static void path_join(char *out, size_t size, const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la + 1 + lb + 1 > size) {
        fprintf(stderr, "pty_test: path too long: %s/%s\n", a, b);
        exit(2);
    }
    memcpy(out, a, la);
    out[la] = '/';
    memcpy(out + la + 1, b, lb + 1);
}

static void make_dirs(const char *path) {
    char buf[8192];
    size_t n = strlen(path);
    if (n >= sizeof(buf)) {
        fprintf(stderr, "pty_test: path too long: %s\n", path);
        exit(2);
    }
    memcpy(buf, path, n + 1);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    mkdir(buf, 0755);
}

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        perror(path);
        exit(2);
    }
    fputs(text, f);
    fclose(f);
}

/* Key names for typed(); anything else is sent as text. */
static const char *key(const char *name) {
    static const struct { const char *name, *seq; } KEYS[] = {
        { "left", "\033[D" }, { "right", "\033[C" }, { "up", "\033[A" }, { "down", "\033[B" },
        { "home", "\033[H" }, { "end", "\033[F" }, { "home1", "\033[1~" }, { "end4", "\033[4~" },
        { "del", "\033[3~" }, { "altb", "\033b" }, { "altf", "\033f" }, { "cleft", "\033[1;5D" },
        { "C-a", "\001" }, { "C-b", "\002" }, { "C-c", "\003" }, { "C-d", "\004" }, { "C-e", "\005" },
        { "C-f", "\006" }, { "C-k", "\013" }, { "C-r", "\022" }, { "C-u", "\025" }, { "C-w", "\027" },
        { "C-y", "\031" }, { "bs", "\177" }, { NULL, NULL }
    };
    for (int i = 0; KEYS[i].name; i++) {
        if (strcmp(KEYS[i].name, name) == 0) return KEYS[i].seq;
    }
    return name;
}

/* Types each part (text, or a key name), then Enter. NULL ends the list. */
static void typed(Session *s, ...) {
    va_list ap;
    va_start(ap, s);
    for (const char *p; (p = va_arg(ap, const char *)) != NULL;) ssend(s, key(p), 0.05);
    va_end(ap);
    ssend(s, "\r", 0.3);
}

/* The program printed text as a line of output (not just echoed it). */
static int got(Session *s, const char *text) {
    char want[8300];
    snprintf(want, sizeof(want), "\n%s\r\n", text);
    return expect(s, want, 2.0);
}

/* ---- 3.2 startup files ---- */

static void test_startup(void) {
    char home[4200], file[4300];
    path_join(home, sizeof(home), tmp, "home");
    make_dirs(home);
    path_join(file, sizeof(file), home, ".acshrc");
    write_file(file, "alias fromrc='echo rc-alias-works'\n");

    Session *s = start(NULL, clean_env(home, NULL, NULL), NULL);
    ssend(s, "fromrc\n", 0.4);
    check("a plain interactive shell (docker exec -it, lxc exec) reads ~/.acshrc", expect(s, "rc-alias-works", 3), s);
    ssend(s, "ll >/dev/null && echo default-ll-ok\n", 0.4);
    check("the default aliases are there too", expect(s, "default-ll-ok", 3), s);
    finish(s);

    path_join(file, sizeof(file), home, "envfile");
    write_file(file, "alias fromenv='echo env-alias-works'\n");
    s = start(NULL, clean_env(home, "ENV=$HOME/envfile", NULL), NULL);
    ssend(s, "fromenv; fromrc\n", 0.4);
    check("$ENV is expanded and read after ~/.acshrc",
          expect(s, "env-alias-works", 3) && expect(s, "rc-alias-works", 3), s);
    finish(s);

    if (geteuid() != 0) {
        skip("/etc/acshrc is read by every interactive shell", "needs root");
        skip("aliases from /etc/acshrc survive su to another user", "needs root");
        return;
    }
    if (access("/etc/acshrc", F_OK) == 0) {
        skip("/etc/acshrc tests", "/etc/acshrc already exists here");
        return;
    }
    write_file("/etc/acshrc", "alias sysalias='echo system-alias-works'\n");
    s = start(NULL, clean_env(home, NULL, NULL), NULL);
    ssend(s, "sysalias\n", 0.4);
    check("/etc/acshrc is read by every interactive shell", expect(s, "system-alias-works", 3), s);
    finish(s);
    const char *su = NULL;
    static const char *places[] = { "/bin/su", "/usr/bin/su", "/sbin/su", "/usr/sbin/su", NULL };
    for (int i = 0; places[i]; i++) {
        if (access(places[i], X_OK) == 0) { su = places[i]; break; }
    }
    if (su == NULL) {
        skip("aliases from /etc/acshrc survive su to another user", "no su");
    } else {
        char *argv[] = { (char *)su, "-s", acsh, "nobody", NULL };
        s = start(argv, clean_env("/", NULL, NULL), "/");
        ssend(s, "sysalias; id -un\n", 0.4);
        check("aliases from /etc/acshrc survive su to another user",
              expect(s, "system-alias-works", 3) && expect(s, "nobody", 3), s);
        finish(s);
    }
    unlink("/etc/acshrc");
}

/* ---- 3.4 prompt ---- */

static void test_prompt(void) {
    char home[4200], git[4300], repo[4300], head[4400];
    path_join(home, sizeof(home), tmp, "phome");
    path_join(git, sizeof(git), home, "proj/.git");
    path_join(repo, sizeof(repo), home, "proj/sub");
    make_dirs(git);
    make_dirs(repo);
    path_join(head, sizeof(head), git, "HEAD");
    write_file(head, "ref: refs/heads/feature-x\n");
    Session *s = start(NULL, clean_env(home, NULL, NULL), repo);
    check("the default prompt shows ~ and the git branch", expect(s, "acsh:~/proj/sub (feature-x)$ ", 3), s);
    ssend(s, "cd /\n", 0.4);
    check("outside a repository there is no branch", expect(s, "acsh:/$ ", 3), s);
    ssend(s, "PS1='[\\?|$((6*7))|\\W]\\$ '; false\n", 0.4);
    check("PS1 escapes and $((...)) expand: last status, last dir part", expect(s, "[1|42|/]$ ", 3), s);
    ssend(s, "PS2='more> '; if true\n", 0.4);
    check("PS2 is used for continuation lines", expect(s, "more> ", 3), s);
    ssend(s, "then echo done; fi\n", 0.4);
    ssend(s, "export PS1; env | grep -c '^PS1='\n", 0.4);
    check("PS1 is never exported, even after export PS1", expect(s, "\r\n0\r\n", 3), s);
    finish(s);
}

/* ---- 3.7 line editor ---- */

static void test_lineedit(void) {
    char home[4200];
    path_join(home, sizeof(home), tmp, "lhome");
    make_dirs(home);
    Session *s = start(NULL, clean_env(home, NULL, NULL), NULL);

    typed(s, "echo OUT:abc", "left", "left", "X", NULL);
    check("Left moves back, typing inserts", got(s, "OUT:aXbc"), s);
    typed(s, "OUT:hello", "C-a", "echo ", NULL);
    check("Ctrl-A goes to the start", got(s, "OUT:hello"), s);
    typed(s, "echo OUT:a", "C-a", "C-e", "b", NULL);
    check("Ctrl-E goes to the end", got(s, "OUT:ab"), s);
    typed(s, "OUT:x", "home", "echo ", "end", "y", "home1", "end4", "z", NULL);
    check("Home and End (ESC [H, ESC [F, ESC [1~, ESC [4~)", got(s, "OUT:xyz"), s);
    typed(s, "echo OUT:ac", "C-b", "C-b", "C-f", "b", NULL);
    check("Ctrl-B and Ctrl-F", got(s, "OUT:abc"), s);
    typed(s, "echo OUT:abxc", "left", "bs", "del", "c", "left", "left", NULL);
    check("Backspace and Delete", got(s, "OUT:abc"), s);
    typed(s, "rubbish", "C-u", "echo OUT:clean", NULL);
    check("Ctrl-U cuts to the start", got(s, "OUT:clean"), s);
    typed(s, "echo OUT:one two", "C-w", "three", NULL);
    check("Ctrl-W cuts the word before", got(s, "OUT:one three"), s);
    typed(s, "echo OUT:keep cut", "left", "left", "left", "left", "C-k", NULL);
    check("Ctrl-K cuts to the end", got(s, "OUT:keep"), s);
    typed(s, "echo OUT:ab cd", "C-w", "C-y", "C-y", NULL);
    check("Ctrl-Y pastes the last cut", got(s, "OUT:ab cdcd"), s);
    typed(s, "echo OUT:aa cc", "altb", "bb ", "altf", "!", NULL);
    check("Alt-B and Alt-F move by words", got(s, "OUT:aa bb cc!"), s);
    typed(s, "echo OUT:x z", "cleft", "y ", NULL);
    check("Ctrl-Left moves by words", got(s, "OUT:x y z"), s);
    typed(s, "echo OUT:\xc3\xa9\xc3\xa8", "left", "a", NULL);
    check("UTF-8: the cursor moves by characters", got(s, "OUT:\xc3\xa9" "a\xc3\xa8"), s);

    typed(s, "PS1=\"$(printf '%0120d' 0)\\$ \"", NULL);
    typed(s, "echo OUT:wide", "left", "left", "-", NULL);
    check("a prompt wider than the screen still leaves room to edit", got(s, "OUT:wi-de"), s);
    typed(s, "PS1='$ '", NULL);
    char longcmd[400], longout[301];
    memset(longout, 'x', 300);
    longout[300] = '\0';
    snprintf(longcmd, sizeof(longcmd), "echo OUT:%s", longout);
    typed(s, longcmd, NULL);
    snprintf(longcmd, sizeof(longcmd), "OUT:%s", longout);
    check("a line wider than the screen scrolls and runs whole", got(s, longcmd), s);

    typed(s, "echo OUT:first", NULL);
    typed(s, "echo OUT:second", NULL);
    typed(s, "up", "up", NULL);
    check("Up recalls history", got(s, "OUT:first"), s);
    typed(s, "echo OUT:draft", "up", "down", NULL);
    check("Down comes back to the line being typed", got(s, "OUT:draft"), s);
    typed(s, "echo OUT:needle-42", NULL);
    typed(s, "echo OUT:other", NULL);
    typed(s, "C-r", "needle", NULL);
    check("Ctrl-R finds an older command and Enter runs it", got(s, "OUT:needle-42"), s);

    ssend(s, "echo OUT:not-run", 0.1);
    ssend(s, key("C-c"), 0.3);
    typed(s, "echo OUT:st=$?", NULL);
    typed(s, "echo OUT:after-c", NULL);
    check("Ctrl-C drops the line (status 130)",
          got(s, "OUT:after-c") && got(s, "OUT:st=130") && !contains(s, "\nOUT:not-run"), s);
    typed(s, "if true", NULL);
    ssend(s, key("C-c"), 0.3);
    typed(s, "echo OUT:recovered", NULL);
    check("Ctrl-C on a continuation line cancels the whole command",
          got(s, "OUT:recovered") && !contains(s, "syntax error"), s);
    typed(s, "set -o ignoreeof", NULL);
    ssend(s, key("C-d"), 0.3);
    check("Ctrl-D on an empty line with ignoreeof does not exit", expect(s, "Use \"exit\"", 3), s);
    typed(s, "set +o ignoreeof", NULL);
    ssend(s, key("C-d"), 0.5);
    pid_t done = waitpid(s->pid, NULL, WNOHANG);
    for (int i = 0; i < 10 && done == 0; i++) {
        sread(s, 0.1);
        done = waitpid(s->pid, NULL, WNOHANG);
    }
    check("Ctrl-D on an empty line ends the shell", done == s->pid, s);
    if (done != s->pid) {
        kill(s->pid, SIGKILL);
        waitpid(s->pid, NULL, 0);
    }
    close(s->fd);
    free(s->out);
    free(s);

    s = start(NULL, clean_env(home, "TERM=dumb", NULL), NULL);
    ssend(s, "echo OUT:$((2+3))\r", 0.4);
    check("TERM=dumb reads plain lines (no editor)", expect(s, "OUT:5", 3) && !contains(s, "\033[K"), s);
    finish(s);
}

/* ---- 3.8 completion ---- */

static void test_complete(void) {
    char work[4200], p[4400], want[4600];
    path_join(work, sizeof(work), tmp, "cwork");
    path_join(p, sizeof(p), work, "src/deep");
    make_dirs(p);
    path_join(p, sizeof(p), work, "sample dir");
    make_dirs(p);
    path_join(p, sizeof(p), work, "srcfile.c");
    write_file(p, "");
    path_join(p, sizeof(p), work, "sample dir/inner.txt");
    write_file(p, "");
    Session *s = start(NULL, clean_env(work, NULL, NULL), work);
    typed(s, "cd sr\t", "; echo OUT:$PWD", NULL);
    snprintf(want, sizeof(want), "OUT:%s/src", work);
    check("cd sr<Tab> completes to the directory, not the file", got(s, want), s);
    typed(s, "cd ..", NULL);
    typed(s, "echo OUT:$PA\t", NULL);
    check("$PA<Tab> completes to $PATH", got(s, "OUT:/usr/sbin:/usr/bin:/sbin:/bin"), s);
    typed(s, "echo OUT:${HOM\t", NULL);
    snprintf(want, sizeof(want), "OUT:%s", work);
    check("${HOM<Tab> completes to ${HOME}", got(s, want), s);
    typed(s, "ech\tOUT:cmd", NULL);
    check("a command name completes (builtin)", got(s, "OUT:cmd"), s);
    typed(s, "my_long_function_name() { echo OUT:fn; }", NULL);
    typed(s, "my_long_fun\t", NULL);
    check("a function name completes", got(s, "OUT:fn"), s);
    typed(s, "alias zz_unique_alias='echo OUT:alias'", NULL);
    typed(s, "zz_uniq\t", NULL);
    check("an alias name completes", got(s, "OUT:alias"), s);
    typed(s, "echo sam\tinn\t", NULL);
    check("a name with a space is escaped, and completion goes on inside it", got(s, "sample dir/inner.txt"), s);
    ssend(s, "apk upg\t", 0.3);
    check("apk upg<Tab> completes to apk upgrade", expect(s, "apk upgrade ", 3), s);
    ssend(s, key("C-u"), 0.2);
    ssend(s, "ls s\t", 0.4);
    check("when Tab adds nothing, the choices are listed",
          expect(s, "sample dir/", 3) && expect(s, "srcfile.c", 3), s);
    finish(s);
}

/* ---- 3.9 history suggestions ---- */

static void test_suggest(void) {
    char home[4200];
    path_join(home, sizeof(home), tmp, "shome");
    make_dirs(home);
    Session *s = start(NULL, clean_env(home, NULL, NULL), NULL);
    typed(s, "echo OUT:suggest-me-please", NULL);
    s->len = 0;
    ssend(s, "echo OUT:sug", 0.4);
    check("suggestions are off by default", !contains(s, "\033[90m"), s);
    ssend(s, key("C-u"), 0.2);
    typed(s, "set -o suggest", NULL);
    s->len = 0;
    ssend(s, "echo OUT:sug", 0.4);
    check("with set -o suggest, the rest of a history match shows in grey",
          contains(s, "\033[90mgest-me-please"), s);
    ssend(s, key("right"), 0.2);
    ssend(s, "\r", 0.4);
    check("Right arrow accepts the suggestion", got(s, "OUT:suggest-me-please"), s);
    s->len = 0;
    ssend(s, "echo OUT:zzz-no-match", 0.4);
    /* only what was drawn after the last keystroke counts */
    const char *after = s->out, *hit;
    while ((hit = memmem(after, s->len - (size_t)(after - s->out), "zzz-no-match", 12)) != NULL) after = hit + 12;
    check("the suggestion goes away when nothing matches",
          memmem(after, s->len - (size_t)(after - s->out), "\033[90m", 5) == NULL, s);
    ssend(s, "\r", 0.3);
    finish(s);
}

/* ---- main ---- */

static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st; (void)flag; (void)ftw;
    return remove(path);
}

int main(int argc, char **argv) {
    const char *path = "./acsh";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--minimal") == 0) minimal = 1;
        else path = argv[i];
    }
    if (realpath(path, acsh) == NULL || access(acsh, X_OK) != 0) {
        fprintf(stderr, "pty_test: %s: not an executable acsh\n", path);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    snprintf(tmp, sizeof(tmp), "/tmp/acsh-pty-XXXXXX");
    if (mkdtemp(tmp) == NULL) {
        perror("pty_test: mkdtemp");
        return 2;
    }
    static const struct { const char *name; void (*run)(void); int editor; } TESTS[] = {
        { "complete", test_complete, 1 }, { "lineedit", test_lineedit, 1 },
        { "prompt", test_prompt, 0 }, { "startup", test_startup, 0 }, { "suggest", test_suggest, 1 },
    };
    for (size_t i = 0; i < sizeof(TESTS) / sizeof(TESTS[0]); i++) {
        printf("[%s]\n", TESTS[i].name);
        if (minimal && TESTS[i].editor) {
            char what[64];
            snprintf(what, sizeof(what), "%s tests", TESTS[i].name);
            skip(what, "MINIMAL build has no line editor");
            continue;
        }
        TESTS[i].run();
    }
    if (minimal) {
        printf("[minimal]\n");
        Session *s = start(NULL, clean_env(tmp, NULL, NULL), NULL);
        ssend(s, "echo OUT:$((40+2))\r", 0.4);
        check("a MINIMAL build reads plain lines on a terminal", expect(s, "OUT:42", 3) && !contains(s, "\033[K"), s);
        finish(s);
    }
    nftw(tmp, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
    printf("pty summary: %d pass, %d fail, %d skipped\n", npass, nfail, nskip);
    return nfail ? 1 : 0;
}
