#!/bin/sh

APPPATH="./build/adduco"
# set detach key explicitly in case it was changed in config.h
CLI_OPTS="-e ^\\"

# Terminal sequences written by adduco on attach and when the terminal is
# given back; keep in sync with terminal_reset_modes() and
# terminal_reset_margins() in adduco.c.
TERM_RESET="$(printf '\033[0m\033[?1l\033>\033[?25h\033[?1000l\033[?1002l\033[?1003l\033[?1005l\033[?1006l\033[?2004l\033[?7h')"
TERM_PROLOG="$(printf '\033[?1049h\033[H%s\033[r' "$TERM_RESET")"
TERM_EPILOG="$(printf '\033[?1049l%s' "$TERM_RESET")"

[ ! -z "$1" ] && APPPATH="$1"
[ ! -x "$APPPATH" ] && echo "usage: $0 /path/to/app" && exit 1

APPNAME="$(basename "$APPPATH")"

TESTS_OK=0
TESTS_RUN=0

detach() {
	sleep 1
	printf ""
}

expected_prolog() {
	printf "%s" "$TERM_PROLOG"
	echo
	echo "$APPNAME: $1 - attached"
	echo
}

# $1 => session-name, $2 => exit status
expected_epilog() {
	printf "%s$APPNAME: %s - session terminated with exit status %s\n" "$TERM_EPILOG" "$1" "$2"
}

# $1 => session-name, $2 => cmd to run
expected_attached_output() {
	expected_prolog "$1"
	$2
	expected_epilog "$1" $?
}

# $1 => session-name, $2 => cmd to run
expected_detached_output() {
	expected_prolog "$1"
	$2 >/dev/null 2>&1
	expected_epilog "$1" $?
}

check_environment() {
	[ "`$APPPATH -s | wc -l`" -gt 1 ] && echo "$APPNAME" session exists && exit 1;
	pgrep -x "$APPNAME" && echo $APPNAME process exists && exit 1;
	return 0;
}

test_non_existing_command() {
	check_environment || return 1;
	$APPPATH -c test ./non-existing-command >/dev/null 2>&1
	check_environment || return 1;
}

# $1 => session-name, $2 => command to execute
run_test_attached() {
	check_environment || return 1;

	local name="$1"
	local cmd="$2"
	local output="$name.out"
	local output_expected="$name.expected"

	TESTS_RUN=$((TESTS_RUN + 1))
	echo -n "Running test attached: $name "
	expected_attached_output "$name" "$cmd" > "$output_expected" 2>&1

	if $APPPATH -c "$name" $cmd 2>&1 | sed 's/.$//' > "$output" && sleep 1 &&
	   diff -u "$output_expected" "$output" && check_environment; then
		rm "$output" "$output_expected"
		TESTS_OK=$((TESTS_OK + 1))
		echo "OK"
		return 0
	else
		echo "FAIL"
		return 1
	fi
}

# $1 => session-name, $2 => command to execute
run_test_detached() {
	check_environment || return 1;

	local name="$1"
	local cmd="$2"
	local output="$name.out"
	local output_expected="$name.expected"

	TESTS_RUN=$((TESTS_RUN + 1))
	echo -n "Running test detached: $name "
	expected_detached_output "$name" "$cmd" > "$output_expected" 2>&1

	if $APPPATH -n "$name" $cmd >/dev/null 2>&1 && sleep 1 &&
	   $APPPATH -a "$name" 2>&1 | sed 's/.$//' > "$output" &&
	   diff -u "$output_expected" "$output" && check_environment; then
		rm "$output" "$output_expected"
		TESTS_OK=$((TESTS_OK + 1))
		echo "OK"
		return 0
	else
		echo "FAIL"
		return 1
	fi
}

# $1 => session-name, $2 => command to execute
run_test_attached_detached() {
	check_environment || return 1;

	local name="$1"
	local cmd="$2"
	local output="$name.out"
	local output_expected="$name.expected"

	TESTS_RUN=$((TESTS_RUN + 1))
	echo -n "Running test: $name "
	$cmd >/dev/null 2>&1
	expected_epilog "$name" $? > "$output_expected" 2>&1

	if detach | $APPPATH $CLI_OPTS -c "$name" $cmd >/dev/null 2>&1 && sleep 3 &&
	   $APPPATH -a "$name" 2>&1 | tail -1 | sed 's/.$//' > "$output" &&
	   diff -u "$output_expected" "$output" && check_environment; then
		rm "$output" "$output_expected"
		TESTS_OK=$((TESTS_OK + 1))
		echo "OK"
		return 0
	else
		echo "FAIL"
		return 1
	fi
}

test_non_existing_command || echo "Execution of non existing command FAILED"

run_test_attached "awk" "awk 'BEGIN {for(i=1;i<=1000;i++) print i}'"
run_test_detached "awk" "awk 'BEGIN {for(i=1;i<=1000;i++) print i}'"

run_test_attached "false" "false"
run_test_detached "false" "false"

run_test_attached "true" "true"
run_test_detached "true" "true"

cat > exit-status.sh <<-EOT
	#!/bin/sh
	exit 42
EOT
chmod +x exit-status.sh

run_test_attached "exit-status" "./exit-status.sh"
run_test_detached "exit-status" "./exit-status.sh"

rm ./exit-status.sh

cat > long-running.sh <<-EOT
	#!/bin/sh
	echo Start
	date
	sleep 3
	echo Hello World
	sleep 3
	echo End
	date
	exit 1
EOT
chmod +x long-running.sh

run_test_attached_detached "attach-detach" "./long-running.sh"

rm ./long-running.sh

[ $TESTS_OK -eq $TESTS_RUN ]
