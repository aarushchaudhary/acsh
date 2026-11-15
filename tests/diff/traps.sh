# trap: any signal by name or number, ignore, reset, EXIT, ERR, listing, subshells, wait.
trap 'echo got-term' TERM; kill -TERM $$; echo after-term
trap 'echo t-usr1' 10; kill -USR1 $$
trap '' HUP; kill -HUP $$; echo survived-ignored-hup
trap; trap - TERM HUP 10; trap
trap 'echo in-trap $?; false' USR2; true; kill -USR2 $$; echo "status after trap $?"
trap 'echo parent' USR1; (trap; kill -USR1 $$; sleep 0.1; echo child-after); echo subshell-done
trap 'echo bad' BOGUS; echo "bad signal st=$?"
trap 'echo caught' USR1; (sleep 0.3; kill -USR1 $$) & wait; echo "wait st=$?"
# (ERR after the wait: ash's wait returns 1 instead of 128+N once an ERR trap has run)
trap 'echo err-trap' ERR; false; if false; then true; fi; trap - ERR
printf 'kill -USR1 $$\necho sourced-after\n' > src.sh; . ./src.sh
f() { trap 'echo exit-trap-from-f' EXIT; }; f; echo after-f
