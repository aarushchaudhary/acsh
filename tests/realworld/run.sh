#!/bin/sh
# Real-world scripts under acsh and busybox ash, compared: the system's
# /etc/profile, every init script in /etc/init.d (OpenRC on Alpine),
# abuild and its functions.sh, and the ./configure of a small autotools
# project generated here. Each must give the same exit status and the
# same output under both shells. A part whose input is missing (no
# OpenRC, no abuild, no autoconf) is reported SKIP.
#
#   tests/realworld/run.sh [path/to/acsh]     (ASH="busybox ash" by default)
#
# Init scripts are only sourced (to run depend() with OpenRC's helpers
# stubbed out) when they are OpenRC scripts, whose top level just sets
# variables; any other init script is only syntax-checked with -n.

ACSH=${1:-./acsh}
ACSH=$(cd "$(dirname "$ACSH")" && pwd)/$(basename "$ACSH")
ASH=${ASH:-busybox ash}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
pass=0; fail=0; skip=0

result() { # result NAME: compares $work/a.out with $work/b.out
    if cmp -s "$work/a.out" "$work/b.out"; then
        pass=$((pass + 1)); echo "PASS:  $1"
    else
        fail=$((fail + 1)); echo "FAIL:  $1"
        diff -u "$work/a.out" "$work/b.out" | sed 's/^/    /' | head -30
    fi
}
skipped() { skip=$((skip + 1)); echo "SKIP:  $1 ($2)"; }

# both NAME CMD...: runs CMD under each shell (the shell is $SH), keeps
# stdout and the exit status
both() {
    name=$1; shift
    SH=$ASH;  export SH; (eval "$*") >"$work/a.out" 2>/dev/null; echo "exit=$?" >>"$work/a.out"
    SH=$ACSH; export SH; (eval "$*") >"$work/b.out" 2>/dev/null; echo "exit=$?" >>"$work/b.out"
    result "$name"
}

echo "[profile]"
# A busybox ash built to prefer its own applets (Ubuntu's) runs its
# built-in run-parts, ls and so on instead of the system's: the profile
# then takes other paths in ash, and comparing would mean nothing.
if [ "$($ASH -c 'command -v ls' 2>/dev/null)" = ls ]; then
    skipped "/etc/profile" "this busybox ash runs its own applets instead of the system's commands"
elif [ -r /etc/profile ]; then
    mkdir -p "$work/home"
    both "/etc/profile in a login shell: status, no errors, the environment it sets" \
        'HOME=$work/home env -i HOME=$work/home PATH=/usr/sbin:/usr/bin:/sbin:/bin TERM=dumb $SH -l -c '"'"'
            echo "PATH=$PATH"; echo "LANG=$LANG"; echo "CHARSET=$CHARSET"; echo "PAGER=$PAGER"
            case $PS1 in "" | "\$ ") ;; *) echo "PS1=$PS1" ;; esac   # only a PS1 the profile set
            echo "umask=$(umask)"'"'"' 2>&1 | grep -v "^ash:\|^acsh:" ; true'
    both "/etc/profile writes nothing to stderr" \
        'env -i HOME=$work/home PATH=/usr/sbin:/usr/bin:/sbin:/bin TERM=dumb $SH -l -c : 2>&1 >/dev/null | wc -c'
else
    skipped "/etc/profile" "none here"
fi

echo "[init scripts]"
n=0
for f in /etc/init.d/*; do
    [ -f "$f" ] || continue
    n=$((n + 1))
    both "sh -n $f" '$SH -n "'"$f"'"'
    if head -1 "$f" | grep -q openrc-run; then
        both "OpenRC $f: depend() and its settings" '$SH -c '"'"'
            for fn in need use want after before provide keyword config; do eval "$fn() { echo $fn \"\$@\"; }"; done
            for fn in ebegin eend einfo ewarn eerror veinfo vewarn checkpath; do eval "$fn() { :; }"; done
            RC_SVCNAME=$(basename "$1"); SVCNAME=$RC_SVCNAME
            . "$1" >/dev/null 2>&1
            command -v depend >/dev/null 2>&1 && depend
            echo "name=$name command=$command args=$command_args pidfile=$pidfile user=$command_user bg=$command_background"
            echo "extra=$extra_commands started=$extra_started_commands"
            for fn in start stop status reload start_pre stop_post; do command -v $fn >/dev/null 2>&1 && echo "has $fn"; done
        '"'"' sh "'"$f"'"'
    fi
done
[ "$n" -eq 0 ] && skipped "init scripts" "/etc/init.d is empty or missing"

echo "[abuild]"
FUNCS=/usr/share/abuild/functions.sh
if [ -r "$FUNCS" ] && command -v abuild >/dev/null 2>&1; then
    both "sh -n $(command -v abuild)" '$SH -n "$(command -v abuild)"'
    both "abuild -h" '$SH "$(command -v abuild)" -h 2>&1 | sed "s/^[a-z]*sh: //"'
    both "functions.sh: its helpers" '$SH -c '"'"'
        . '"$FUNCS"'
        readconfig 2>/dev/null; echo "CARCH=$CARCH CHOST=$CHOST CBUILD=$CBUILD"
        for a in x86_64 aarch64 armv7 x86 ppc64le s390x riscv64; do echo "$a: $(arch_to_hostspec $a)"; done
        for h in x86_64-alpine-linux-musl aarch64-alpine-linux-musl armv7-alpine-linux-musleabihf; do echo "$h: $(hostspec_to_arch $h)"; done
        command -v hostspec_to_libc >/dev/null && hostspec_to_libc x86_64-alpine-linux-musl
        set | grep -c "^[A-Z_]*=" >/dev/null; echo done
    '"'"''
else
    skipped "abuild" "abuild is not installed"
fi

echo "[configure]"
if command -v autoreconf >/dev/null 2>&1 && command -v cc >/dev/null 2>&1; then
    proj=$work/proj
    mkdir -p "$proj"
    cat >"$proj/configure.ac" <<'EOF'
AC_INIT([acshdemo], [1.0], [none@example.invalid])
AC_CONFIG_SRCDIR([demo.c])
AC_CONFIG_HEADERS([config.h])
AC_PROG_CC
AC_PROG_INSTALL
AC_CHECK_HEADERS([stdio.h stdlib.h string.h unistd.h sys/stat.h nonexistent_header_xyz.h])
AC_CHECK_FUNCS([strdup getline nonexistent_function_xyz])
AC_CHECK_SIZEOF([long])
AC_C_BIGENDIAN
AC_ARG_ENABLE([fancy], [AS_HELP_STRING([--enable-fancy], [a test option])], [fancy=$enableval], [fancy=no])
AS_IF([test "x$fancy" = xyes], [AC_DEFINE([FANCY], [1], [fancy mode])])
AC_SUBST([fancy])
AC_CONFIG_FILES([Makefile])
AC_OUTPUT
EOF
    printf 'CC = @CC@\nCFLAGS = @CFLAGS@\nFANCY = @fancy@\nprefix = @prefix@\nall:\n\t$(CC) $(CFLAGS) -o demo demo.c\n' >"$proj/Makefile.in"
    printf '#include "config.h"\nint main(void) { return 0; }\n' >"$proj/demo.c"
    if (cd "$proj" && autoreconf -i >/dev/null 2>&1); then
        for opts in "" "--enable-fancy --prefix=/opt/x"; do
            both "./configure $opts: status, config.h and Makefile" \
                'rm -rf $work/c && cp -r $work/proj $work/c && cd $work/c && CONFIG_SHELL=$SH $SH ./configure '"$opts"' >/dev/null 2>&1; echo "configure=$?"; grep -v "^/\*\|^ \*" config.h; cat Makefile'
        done
        both "./configure --help" 'cd $work/proj && $SH ./configure --help | head -40'
    else
        skipped "configure" "autoreconf failed"
    fi
else
    skipped "configure" "autoconf or a C compiler is not installed"
fi

echo "realworld summary: $pass pass, $fail fail, $skip skipped"
[ "$fail" -eq 0 ]
