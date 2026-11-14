# return: in loops, if, case, modulo 256, bare, and at the top level of a script.
f() {
  return 3
  echo no
}
f; echo "st=$?"
g() {
  return
}
false; g; echo "bare=$?"
h() {
  for i in 1 2 3
  do
    while true
    do
      return 7
    done
    echo no-for
  done
  echo no-h
}
h; echo "nested=$?"
k() {
  if return 4
  then
    echo no
  fi
  echo no2
}
k; echo "if=$?"
m() {
  return 300
}
m; echo "mod=$?"
for i in 1 2
do
  break; echo no-break
done
echo ok
r() {
  case x in
    x) return 9 ;;
  esac
  echo no-case
}
r; echo "case=$?"
return 5
echo not-here
