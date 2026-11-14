# '&' ends a command like ';' does, and runs it in the background.
sleep 0 & echo after-amp
true & false & echo "status $?"
sleep 0.2
