alias
# aliases inside alias text, trailing blanks, no self-recursion
alias e='echo '
alias n='nested '
alias nested='echo deep'
alias w=world
e w
e e w
n w
alias selfref='selfref_fn -x'
selfref_fn() { echo "fn $*"; }
selfref a
alias a1=b1
alias b1=a1
a1 2>/dev/null; echo "alias loop status $?"
alias q="it's"; alias q
alias lsx='ls -x'
alias lsx
