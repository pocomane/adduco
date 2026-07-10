#!/bin/sh

CC="gcc"

CFLAGS="-std=c99 -D_FORTIFY_SOURCE=2"
LDFLAGS="-lc -lutil"

CFLAGS="$CFLAGS -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700" # linux
#CFLAGS="$CFLAGS -D_BSD_SOURCE -D__BSD_VISIBLE=1" # freebsd
#CFLAGS="$CFLAGS -D_BSD_SOURCE" # bsd
#CFLAGS="$CFLAGS -D_DARWIN_C_SOURCE" ;; # darwin
#CFLAGS="$CFLAGS -D_ALL_SOURCE" ;; # aix

#CFLAGS="$CFLAGS -pipe"
#CFLAGS="$CFLAGS -Os"
#CFLAGS="$CFLAGS -ffunction-sections"
#CFLAGS=-"$CFLAGS fdata-sections"
#LDFLAGS="$LDFLAGS -Wl,--gc-sections"
#CLAGS="$CFLAGS -fPIE"
#CLAGS="$CFLAGS -fstack-protector-all"
#LDFLAGS="$LDFLAGS -Wl,-z,now"
#LDFLAGS="$LDFLAGS -Wl,-z,relro"
#LDFLAGS="$LDFLAGS -pie"

CFLAGS="$CFLAG -DVERSION=\"0.6\" -DNDEBUG"

set -x
mkdir -p build/
$CC $CFLAGS $LDFLAGS -o build/adduco adduco.c

