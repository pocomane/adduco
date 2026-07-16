# adduco a tool for session {at,de}tach support

[adduco](https://github.com/pocomane/adduco) provides
session management i.e. it allows programs to be run independently
from their controlling terminal. That is programs can be detached -
run in the background - and then later reattached. It is licensed under the
[ISC license](https://raw.githubusercontent.com/pocomane/adduco/master/LICENSE)

Refer to the [github tools](https://github.com/pocomane/adduco) for comments,
suggestions, ideas, to a bug report, a patch or something else related to
adduco.

This software is a fork of [abduco](https://github.com/martanne/abduco) than
mainly adds an interactive mode, plus some other small changes.

## Download

Either download the latest [prebuilt packages](https://github.com/pocomane/adduco/releases),
or clone the isource repository [adduco](https://github.com/pocomane/adduco)
and compile it with the `./make.sh` script.

## Quickstart

To get an overview run `adduco` with the help flag:

    $ adduco -h

When running without any arguments, `adduco` will show you an
interactive interface with embeded documentation. From there
you can create, attach, detach, kill, rename session. However
also non-intercative flags are supported.

In order to create a new session `adduco` requires a session name
as well as an command which will be run. If no command is given
the environment variable `$ADDUCO_CMD` is examined and if not set
`sh` is executed. Therefore a new session named *demo* is created with:

    $ adduco -c demo

An arbitrary application can be started as follows:

    $ adduco -c session-name your-application

`CTRL-\` detaches from the active session. This detach key can be
changed by means of the `-e` command line option, `-e ^q` would
for example set it to `CTRL-q`.

To get an overview of existing session run `adduco` with `-s`.

    $ adduco -s
    Active sessions (on host debbook)
    * Thu    2015-03-12 12:05:20    demo-active
    + Thu    2015-03-12 12:04:50    demo-finished
      Thu    2015-03-12 12:03:30    demo

A leading asterisk `*` indicates that at least one client is
connected. A leading plus `+` denotes that the session terminated,
attaching to it will print its exit status.

A session can be reattached by using the `-a` command line option
in combination with the session name which was used during session
creation.

    $ adduco -a demo

## Other features

 * the **session exit status** of the command being run is always kept and
   reported either upon command termination or on reconnection
   e.g. the following works:

        $ adduco -n demo true && adduco -a demo
        adduco: demo: session terminated with exit status 0

 * If you want to make your adduco session available to another user
   in a read only fashion, use [socat](http://www.dest-unreach.org/socat/)
   to proxy the adduco socket in a unidirectional (from the adduco server
   to the client, but not vice versa) way.

   Start your to be shared session, make sure only you have access to
   the `private` directory:

        $ adduco -c /tmp/adduco/private/session

   Then proxy the socket in unidirectional mode `-u` to a directory
   where the desired observers have sufficient access rights:

        $ socat -u unix-connect:/tmp/adduco/private/session unix-listen:/tmp/adduco/public/read-only &

   Now the observers can connect to the read-only side of the socket:

        $ adduco -a /tmp/adduco/public/read-only

   communication in the other direction will not be possible and keyboard
   input will hence be discarded.

 * **socket recreation** by sending the `SIGUSR1` signal to the server
   process. In case the unix domain socket was removed by accident it
   can be recreated. The simplest way to find out the server process
   id is to look for adduco processes which are reparented to the init
   process.

        $ pgrep -P 1 adduco

   After finding the correct PID the socket can be recreated with

        $ kill -USR1 $PID

   If the adduco binary itself has also been deleted, but a session is
   still running, use the following command to bring back the session:

        $ /proc/$PID/exe

 * **improved socket permissions** the session sockets are by default either
   stored in `$HOME/.adduco` or `/tmp/adduco/$USER` in both cases it is
   made sure that only the owner has access to the respective directory.

### Debugging

The protocol content exchanged between client and server can be dumped to
temporary files disabling the `NDEBUG` flag in the `make.sh` script, and
redirecting the `stderr` as follows:

    $ make debug
    $ ./adduco -n debug [command-to-debug] 2> server-log
    $ ./adduco -a debug 2> client-log

If you want to run client and server with one command (e.g. using the `-c`
option) then within `gdb` the option `set follow-fork-mode {child,parent}`
might be useful. Similarly to get a syscall trace `strace -o adduco -ff
[adduco-cmd]` proved to be handy.

