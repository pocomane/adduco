#!/bin/sh
set -x
set -e

export SRCDIR="$PWD"
export PATH="$SRCDIR/build:$PATH"
mkdir -p /tmp/generate_demo_gif
cd /tmp/generate_demo_gif

apk add asciinema expect
apk add ttf-dejavu
if [ ! -f "demo.agg" ] ; then
	curl -L https://github.com/asciinema/agg/releases/latest/download/agg-x86_64-unknown-linux-musl -o demo.agg
	chmod ugo+x demo.agg
fi
rm -f demo.cast

cat > demo.exp <<'EOF'
#!/usr/bin/expect -f

# asciinema binds Ctrl-\ to pause/resume recording, but that is also
# adduco's detach key: use a throw-away config without that binding so the
# key reaches adduco and the recording is not paused
set cfg [file join /tmp "asciinema-demo-[pid]"]
file mkdir $cfg
set fp [open [file join $cfg config] w]
puts $fp "\[record\]"
puts $fp "pause_key ="
close $fp
set env(ASCIINEMA_CONFIG_HOME) $cfg

set send_human {.03 .05 1 .03 .05}

proc sleeph {} {
  sleep [expr {rand() * 0.1 + 0.9}]
}

proc sendh {string} {
  send -h $string
  sleeph
}

proc sendr {} {
  send "\r"
  sleeph
}

proc send_ctrlbs {} {
  send "\034"
  sleeph
}

proc send_down {} {
  send "\033\[B"
  sleeph
}

spawn asciinema rec -q -i 1.3 --cols 110 --rows 12 -c sh demo.cast
sleep 0.7

# open the interactive session list
sendh "adduco"
sendr

# create a session, with the default command (a shell)
sendh "c"
sendr
sendh "# hello from inside the session"
sendr
sendh "vi"
sendr
sendh "ihello!"
sendr

# detach: the application keeps running in its session
send_ctrlbs

# create a second session running a full screen application: the command and
# its arguments are asked separately
sendh "c"
sendh "vi"
sendr
sendh "/etc/os-release"
sendr
sendr

# detach and give the randomly named shell session a friendlier name
send_ctrlbs
sendh "m"
sendh "work"
sendr

# move the selection and reattach the first session: the application is
# still there exactly as we left it
send_down
sendr
send_ctrlbs

# quit TUI and reconnect
sendh "q"
sendh "# obviously we can reconnect to adduco TUI and see the open sessions"
sendr
sendh "adduco"
sendr
send_ctrlbs

# kill both sessions (confirm with y) and leave a clean slate
sendh "d"
sendh "y"
sendh "d"
sendh "y"
sendh "q"

sendh "exit"
sendr
expect eof

file delete -force $cfg
EOF

chmod ugo+x demo.exp
./demo.exp
./demo.agg demo.cast demo.gif
rm -f demo.exp demo.cast

cp demo.gif "$SRCDIR/"

