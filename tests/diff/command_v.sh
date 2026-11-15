# command -v: how a name would run. Init scripts and abuild test for
# their own functions this way before calling them.
f() { :; }
alias ll='ls -l'
command -v f; echo "function: $?"
command -v ll; echo "alias: $?"
command -v if; echo "reserved word: $?"
command -v cd; echo "builtin: $?"
command -v no_such_command_xyz; echo "missing: $?"
command -v f cd; echo "two names: $?"
command -v f >/dev/null && echo "the usual test works"
