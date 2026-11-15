# A service script in the style OpenRC runs (written for this test):
# sourced with OpenRC's helpers stubbed, then depend() is called and the
# variables it sets are read back, as tests/realworld does on Alpine.
cat > svc.sh <<'SVC'
#!/sbin/openrc-run
description="An example service"
command=/usr/sbin/exampled
command_args="-f --pid /run/example.pid"
pidfile=/run/example.pid
extra_commands="reload"

depend()
{
	need net localmount
	use logger dns
	after firewall
	keyword -docker -lxc
}

start_pre()
{
	checkpath -d -m 0755 /run/example
}

reload()
{
	ebegin "Reloading $RC_SVCNAME"
	start-stop-daemon --signal HUP --pidfile "$pidfile"
	eend $?
}
SVC
for fn in need use want after before provide keyword; do eval "$fn() { echo $fn \"\$@\"; }"; done
for fn in ebegin eend checkpath; do eval "$fn() { :; }"; done
RC_SVCNAME=svc
. ./svc.sh
command -v depend >/dev/null && depend
echo "command=$command args=$command_args pidfile=$pidfile extra=$extra_commands"
for fn in start start_pre stop reload; do command -v $fn >/dev/null && echo "has $fn"; done
