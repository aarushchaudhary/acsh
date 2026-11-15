# getopts.
# args: -a -b val -c -- rest arg
while getopts ab:c opt; do echo "opt=$opt arg=[$OPTARG] ind=$OPTIND"; done
shift $((OPTIND-1)); echo "rest: $*"
OPTIND=1; set -- -z; getopts :ab o; echo "silent st=$? o=$o arg=[$OPTARG]"
OPTIND=1; set -- -ab; getopts ab o; echo "$o"; getopts ab o; echo "$o $OPTIND"
OPTIND=1; set -- -b; getopts :b: o; echo "missing o=$o arg=[$OPTARG]"
OPTIND=1; getopts x o -x extra; echo "explicit args o=$o"
