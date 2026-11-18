#!/bin/sh
# #############################################################################
# THIS is a container launcher utility. It may contain application
# specific code runned into the container in the `container_prerun` function.
# See github.com/pocomane/corun for more information.
# #############################################################################
# Boilerplate
set -e
SCRDIR="$(cd -P -- "$(dirname "$0")" && pwd)" # for path add /$(basename "$0")

# #############################################################################
# Configuration

HOST_SCRIPT_DIR="$SCRDIR"
CONT_WORK_DIR="/share"
IMAGE_FS="cache/container"
CHECK_FOLDER="$HOST_SCRIPT_DIR/$IMAGE_FS"
CORESYS_FOLDER="coresys" # At root of container filesystem
IMAGE_MODE="${IMAGE_MODE:-release}" # supported values: release (default) or bootstrap (fallback)
GUEST_PRERUN_PATH="/tmp/container_prerun.sh"
GUEST_INITSCRIPT_PATH="/tmp/inittab_script.sh"
GUEST_SHARE_PATH="./"

# Persistent holder for daemonized servers (tmux, abduco, ...). The holder
# keeps a private pid namespace alive so a daemonized server survives the
# (short-lived) client while bwrap still returns immediately. The pid
# namespace stays private: we never drop --unshare-pid.
KEEP_HOLDER_PIDFILE="$HOST_SCRIPT_DIR/$IMAGE_FS/.keep-holder.pid"
KEEP_TMUX_HOSTDIR="$HOST_SCRIPT_DIR/$IMAGE_FS/keep-tmux"
KEEP_TMUX_CONTDIR="/tmp/corun-keep"

CORUN_BWRAP_COMMAND="${CORUN_BWRAP_COMMAND:-$HOST_SCRIPT_DIR/$IMAGE_FS/$CORESYS_FOLDER/bwrap}"

# #############################################################################
# Launcher utility

AUX_RELEASE_IMAGE="https://github.com/pocomane/corun/releases/latest/download/container.tar.gz"
AUX_BUSYBOX_BOOTSTRAP="https://dl-cdn.alpinelinux.org/alpine/v3.20/main/x86_64/busybox-static-1.36.1-r31.apk"
AUX_BUSYBOX_SUBPATH="bin/busybox.static"
AUX_BWRAP_BOOTSTRAP="https://dl-cdn.alpinelinux.org/alpine/edge/main/x86_64/bubblewrap-static-0.11.2-r0.apk"
AUX_BWRAP_SUBPATH="usr/bin/bwrap.static"

download(){
  #wget -O - --no-check-certificate "$1"
  wget -O - "$1"
}

create_container_image_bootstrap(){

  mkdir -p "$HOST_SCRIPT_DIR/$IMAGE_FS/$CORESYS_FOLDER"

  mkdir -p "$HOST_SCRIPT_DIR/$IMAGE_FS/torem"
  cd "$HOST_SCRIPT_DIR/$IMAGE_FS/torem"
  download "$AUX_BUSYBOX_BOOTSTRAP" | tar -xzf -
  cp "$AUX_BUSYBOX_SUBPATH" "$HOST_SCRIPT_DIR/$IMAGE_FS/$CORESYS_FOLDER"/busybox
  cd -
  rm -fR "$HOST_SCRIPT_DIR/$IMAGE_FS"/torem

  mkdir -p "$HOST_SCRIPT_DIR/$IMAGE_FS/torem"
  cd "$HOST_SCRIPT_DIR/$IMAGE_FS/torem"
  download "$AUX_BWRAP_BOOTSTRAP" | tar -xzf -
  cp "$AUX_BWRAP_SUBPATH" "$HOST_SCRIPT_DIR/$IMAGE_FS/$CORESYS_FOLDER"/bwrap
  cd -
  rm -fR "$HOST_SCRIPT_DIR/$IMAGE_FS"/torem

  cd "$HOST_SCRIPT_DIR/$IMAGE_FS/$CORESYS_FOLDER"
  "$CORUN_BWRAP_COMMAND" --bind ../ / -- "/$CORESYS_FOLDER/busybox" --install -s "/$CORESYS_FOLDER/"
  cd -
}

create_container_image_release(){
  mkdir -p "$HOST_SCRIPT_DIR/$IMAGE_FS"
  cd "$HOST_SCRIPT_DIR/$IMAGE_FS"
  download "$AUX_RELEASE_IMAGE" | tar -xzf -
  cd -
}

LAST_FD=5 # 0-2 are standard FD, 3 is used for container_prerun, 4-5 are used for busibox init trick
open_file_descriptor() {
  if [ "$1" = "from_file" ] ; then # Open the file in a FD and retun its number in a variable
    LAST_FD=$((LAST_FD + 1))
    eval "exec ${LAST_FD}<$3"
    eval "$2=${LAST_FD}"
  elif [ "$1" = "to_stdout" ] ; then # Open a FD, redirect it to stdout, and return its number in a variable
    LAST_FD=$((LAST_FD + 1))
    eval "exec ${LAST_FD}>&1"
    eval "$2=${LAST_FD}"
  fi
}

quote() {
    printf '%s\n' "$1" | sed "s/'/'\\\\''/g; 1s/^/'/; \$s/\$/'/"
}

# Print the pid stored in the keeper pidfile (empty if missing/invalid).
corun_keep_holder_pid(){
  _khp=""
  if [ -f "$KEEP_HOLDER_PIDFILE" ] ; then
    _khp="$(cat "$KEEP_HOLDER_PIDFILE" 2>/dev/null)"
    case "$_khp" in ''|*[!0-9]*) _khp="" ;; esac
  fi
  printf '%s' "$_khp"
}

# Return 0 if the persistent holder identified by $1 is alive and joinable.
corun_keep_holder_alive(){
  _kha="$1"
  [ -n "$_kha" ] || return 1
  kill -0 "$_kha" 2>/dev/null || return 1
  [ -e "/proc/$_kha/ns/pid" ] || return 1
  tr '\0' ' ' < "/proc/$_kha/cmdline" 2>/dev/null | grep -q "CORUN_KEEP_HOLDER" || return 1
  return 0
}

# Ensure the persistent holder is running; set CORUN_ATTACH to its pid.
corun_ensure_keep_holder(){
  _khp="$(corun_keep_holder_pid)"
  if corun_keep_holder_alive "$_khp" ; then
    CORUN_ATTACH="$_khp"
    return 0
  fi
  _kh_busybox="$HOST_SCRIPT_DIR/$IMAGE_FS/$CORESYS_FOLDER/busybox"
  _kh_info="${KEEP_HOLDER_PIDFILE}.info.$$.tmp"
  rm -f "$_kh_info"
  LAST_FD=$((LAST_FD + 1))
  _kh_fd=$LAST_FD
  eval "exec ${_kh_fd}>'$_kh_info'"
  "$CORUN_BWRAP_COMMAND" --unshare-pid \
    --bind / / --proc /proc --dev /dev \
    --setenv CORUN_KEEP_HOLDER 1 \
    --info-fd "$_kh_fd" \
    -- "$_kh_busybox" sh -c 'while :; do sleep 86400; done' \
    </dev/null >/dev/null 2>&1 &
  eval "exec ${_kh_fd}>&-"
  _kh_tries=0
  _kh_child=""
  while [ $_kh_tries -lt 50 ] ; do
    if [ -f "$_kh_info" ] ; then
      _kh_child="$(sed -n 's/.*"child-pid"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$_kh_info" 2>/dev/null | head -n 1)"
      if [ -n "$_kh_child" ] && corun_keep_holder_alive "$_kh_child" ; then
        break
      fi
      _kh_child=""
    fi
    _kh_tries=$((_kh_tries + 1))
    sleep 0.1
  done
  rm -f "$_kh_info"
  if [ -z "$_kh_child" ] ; then
    return 1
  fi
  printf '%s\n' "$_kh_child" > "$KEEP_HOLDER_PIDFILE"
  CORUN_ATTACH="$_kh_child"
  return 0
}

container_run(){

  # Put in the CORUN_X environment variable "yes" to enable X server applications
  # Put in the CORUN_ATTACH environment variable a pid to attach to its container

  CONTROOT="$HOST_SCRIPT_DIR/$IMAGE_FS/"
  CMDSIZ="$#"

  # Session multiplexers / daemonizers (tmux, abduco, ...) fork a server
  # that must outlive the (short-lived) client. With --die-with-parent bwrap
  # kills it as soon as the client exits, while without --die-with-parent a
  # private pid namespace keeps bwrap waiting for the server instead of
  # returning immediately like the host does. Such commands get "keep"
  # semantics: they join a persistent holder container that owns a private
  # pid namespace (--unshare-pid is always kept).
  _corun_keep=""
  if [ "$CORUN_KEEP_CHILD" != "" ] ; then
    _corun_keep=1
  fi
  _corun_cmd_base="${1##*/}"
  case "$_corun_cmd_base" in
    tmux|abduco|adduco|dtach|screen|byobu|sesh) _corun_keep=1 ;;
  esac
  if [ "$_corun_keep" != "" ] && [ "$CORUN_ATTACH" = "" ] ; then
    if ! corun_ensure_keep_holder ; then
      echo "corun: cannot start persistent holder for keep-child" >&2
      exit 1
    fi
  fi

  #    note:
  #    - "--cap-add ALL" should be removed for security reason (but it neess some time to find a
  #      good set of capability to keep)
  #    - "--newsession" should be add to avoid guest injecting command in the host terminal (but it
  #      give issues when you want use the container throug the host terminal)
  #    - instead of "--newsession", at least the TIOCSTI capability should be dropped
  #   -  we use --share-net (instead of --unshare-net) since it is most simple way to let
  #      the container interact with the rest of the world.
  #    - we do not use --unshare-all because it will add --unshare-user. We need the
  #      same user namespace to let the host system allow to do stuff with the same privilages of
  #      the caller user (e.g. if called by root, you can bind low port). For example
  #      with --unshare-user there will be issues running as user a container created
  #      as user.
  #

  if [ "$CORUN_EXTRA_BIND_SOURCE" != "" -a "$CORUN_EXTRA_BIND_DESTINATION" != "" ] ; then
    set -- "$@" \
      --bind "$CORUN_EXTRA_BIND_SOURCE"/ "$CORUN_EXTRA_BIND_DESTINATION"/
  fi

  if [ "$CORUN_X" = "yes" ] ; then
    if [ "$XAUTHORITY" = "" ] ; then XAUTHORITY="$HOME/.Xauthority"; fi
    set -- "$@" \
      --setenv DISPLAY "$DISPLAY" \
      --bind "$XAUTHORITY" /root/.Xauthority
    # Other option maybe useful:
    #   --bind /tmp/.X11-unix /tmp/.X11-unix
    #   --bind /dev/dri /dev/dri
    #   --bind /dev/snd /dev/snd
    # To reuse the fonts of the host:
    #   --ro-bind /usr/share/fonts /usr/share/fonts
  fi

  set -- "$@" \
    \
    --cap-add ALL \
    --uid "0" \
    --gid "0" \
    \
    --share-net \
    --unshare-ipc \
    --unshare-uts \
    --unshare-cgroup \
    --clearenv \
    \
    --bind "$CONTROOT" / \
    --dev /dev \
    --tmpfs /run \
    --tmpfs /tmp \
    --bind /sys /sys \
    --proc /proc \
    --tmpfs /dev/shm \
    --bind /sys /sys \
    \
    --chdir "$CONT_WORK_DIR" \
    --setenv HOME /root \
    --setenv PATH "/bin:/usr/bin:/sbin:/usr/sbin:/$CORESYS_FOLDER"  \
    --ro-bind /etc/resolv.conf /etc/resolv.conf \
    --ro-bind /etc/hosts /etc/hosts \
    \
    --bind "$GUEST_SHARE_PATH" "$CONT_WORK_DIR"
    
 # --ro-bind /etc/services /etc/services \

  # Set CORUN_KEEP_CHILD to anything to avoid the whole process subtree
  # to be killed when the main shell exit (useful with tmux, abduco, etc).
  # Keep-child servers live in the persistent holder's pid namespace, so the
  # joining bwrap can exit right away without killing them.
  if [ "$_corun_keep" = "" ] ; then
    set -- "$@" \
      --die-with-parent
  fi

  if [ "$CORUN_ATTACH" = "" ] ; then
    # NOTE : when using --as-pid-1 the luanched process must reap the zombies
    #        so probably you want it to be shor living or someting like init
    # NOTE : it seems that also without --as-pid-1 bwrap do not create a zombi
    #        reaper process with pid 1. Mor tests are needed
    # A fresh container always gets a private pid namespace; keep-child
    # commands never take this path (they join the holder above).
    open_file_descriptor to_stdout FD_INFO
    set -- "$@" \
      --info-fd $FD_INFO \
      --unshare-pid # --unshare-user
  else
    open_file_descriptor to_stdout FD_INFO
    open_file_descriptor from_file FD_PID /proc/"$CORUN_ATTACH"/ns/pid
    # Join the user namespace only when it differs from ours: the keep
    # holder intentionally keeps the host user namespace, and bwrap refuses
    # to "join" it as --userns.
    if [ "$(readlink /proc/self/ns/user)" != "$(readlink /proc/"$CORUN_ATTACH"/ns/user)" ] ; then
      open_file_descriptor from_file FD_USER /proc/"$CORUN_ATTACH"/ns/user
      set -- "$@" \
        --info-fd $FD_INFO \
        --pidns $FD_PID \
        --userns $FD_USER
    else
      set -- "$@" \
        --info-fd $FD_INFO \
        --pidns $FD_PID
    fi
    # Every join gets a fresh /tmp tmpfs, so tmux sockets would be invisible
    # to the next invocation. Share them through a persistent host directory
    # for keep-child sessions.
    if [ "$_corun_keep" != "" ] ; then
      mkdir -p "$KEEP_TMUX_HOSTDIR"
      set -- "$@" \
        --bind "$KEEP_TMUX_HOSTDIR" "$KEEP_TMUX_CONTDIR" \
        --setenv TMUX_TMPDIR "$KEEP_TMUX_CONTDIR"
    fi
  fi

  container_prerun # This opens the FD 3 with code to run at startup of container
  # Then we mount it in a know position
  set -- "$@" \
    --perms 0700 --bind-data 3 "$GUEST_PRERUN_PATH"

  if [ "$CORUN_INIT" = "" -o "$_corun_keep" != "" ] ; then
    # NOTE: the busybox init trick is skipped when keeping children: it
    # requires a private pid namespace to be PID 1, but keep-child joins an
    # existing one, and its 'kill -TERM 1' trap would signal the holder init.
    # Place original command at end of arguments as paramters of container_prerun
    set -- "$@" \
      "$GUEST_PRERUN_PATH"
    i=0 ; while [ "$i" -lt "$CMDSIZ" ] ; do
      arg=$1
      shift
      set -- "$@" "$arg"
      i=$((i + 1))
    done 
  else
    # Busybox init trick
    # Remove the original command, place it in the inittab,
    # and launch busybox init instead. The inittab and other
    # utility file are mounted with --bind-data creating
    # ad-hoc file descriptors
    CMD="" 
    i=0 ; while [ "$i" -lt "$CMDSIZ" ] ; do
      CMD="$CMD $(quote "$1")"
      shift # Remove original command
      i=$((i + 1))
    done
    # Place command in inittab
    exec 4<<EOF
::once:$GUEST_INITSCRIPT_PATH
EOF
    exec 5<<EOFFFFF
#!/coresys/busybox sh
trap 'kill -TERM 1' EXIT INT TERM # Close busybox init when exiting otherwise it will survive to its childen
cd $CONT_WORK_DIR
export PATH='$PATH:/coresys'
"$GUEST_PRERUN_PATH" $CMD
EOFFFFF
    # Launch busybox init instead
    set -- "$@" \
      --as-pid-1 \
      --perms 0777 --bind-data 5 "$GUEST_INITSCRIPT_PATH" \
      --bind-data 4 /etc/inittab \
      busybox init
    # Note : using a true minimal init like tiny o dumb-init would make this
    # simpler, without requiring binding anything, just launch it with the
    # script path at command line. Maybe we can add one of them to the image ?
  fi

  # set -x
  exec "$CORUN_BWRAP_COMMAND" "$@"
}

main(){
  basic_image_check="$CHECK_FOLDER/$CORESYS_FOLDER/container.done"
  if [ ! -e "$basic_image_check" ] ; then
    if [ "$IMAGE_MODE" = "release" ] ; then
      create_container_image_release
    else
      create_container_image_bootstrap
    fi
    touch "$basic_image_check"
  fi
  container_run "$@"
}

# #############################################################################
# Application specific code

container_prerun(){
exec 3<<'EOF'
#!/coresys/busybox sh

set -e

DISTRO_IMAGE="http://dl-cdn.alpinelinux.org/alpine/v3.21/releases/x86_64/alpine-minirootfs-3.21.3-x86_64.tar.gz"

if [ ! -f "/bin/sh" ]; then
  mkdir -p /image-temp
  cd /image-temp
  wget -O - "$DISTRO_IMAGE" | tar -xzf -
  cd -
  cd /
  yes n | cp -Ri image-temp/./ ./
  rm -fR image-temp
  cd -
fi

apk add binutils git gcc musl-dev

if [ "$#" -gt 0 ] ; then
  exec "$@"
else
  exec sh
fi

EOF
}

# #############################################################################
# Main dispatcher
main "$@"

