#!/bin/sh
# Compila flfetch (C + ncurses) como binario i386 para FLinux / Tiny Core.
#
# Receta del taller: la MISMA de FLNauta 1.1 (verificada en la Dell).
#   - Cadena -m32 del host con el sysroot Debian 12 bookworm i386 de
#     ~/workspace/fltubeplus (dl/dev trae la libc.so «recableada» a la
#     glibc local 2.36: requisitos GLIBC <= 2.36, sin rpath).
#   - Enlace contra libncursesw.so.6.5 DE TINY CORE (tc-lib/), por ruta:
#     su SONAME (libncursesw.so.6) queda como DT_NEEDED. La .so de
#     Debian arrastra SUS versiones de simbolos ncurses; la de TC es la
#     que el sistema realmente carga. Ojo: la glibc de TC 17.2 es 2.42
#     y su libncursesw referencia simbolos GLIBC_2.38/2.42: por eso
#     --allow-shlib-undefined (en el invitado los resuelve su libc).
#   - Las cabeceras ncurses salen del host (solo declaraciones).
#
# Salida: ./flfetch-i386. Con BUILD_HOST=1 genera ademas ./flfetch-host
# (x86_64, solo saneamiento en el anfitrion).

set -e
SRC=$(cd "$(dirname "$0")" && pwd)
D=$HOME/workspace/fltubeplus/dl
S=$HOME/workspace/fltubeplus/stage
GXX_LIB=$D/ubu/usr/lib/gcc/x86_64-linux-gnu/13/32
TCW=$SRC/tc-lib/libncursesw.so.6.5

[ -f "$TCW" ] || { echo "FALTA $TCW (libncursesw de Tiny Core)"; exit 1; }
[ -f /usr/include/ncursesw/ncurses.h ] || { echo "FALTAN cabeceras ncursesw del host"; exit 1; }

COMMON="-m32 -O2 -Wall -Wextra -std=gnu11 \
  -I$D/dev/usr/include -I$D/dev/usr/include/i386-linux-gnu -I/usr/include/ncursesw \
  -B$GXX_LIB/ -B$D/dev/usr/lib/i386-linux-gnu/ \
  -L$D/dev/usr/lib/i386-linux-gnu \
  -L$S/usr/lib/i386-linux-gnu -L$S/lib/i386-linux-gnu \
  -L$D/ubu_rt/usr/lib32 \
  -Wl,--no-copy-dt-needed-entries -Wl,--allow-shlib-undefined \
  -Wl,-rpath-link,$S/usr/lib/i386-linux-gnu \
  -Wl,-rpath-link,$S/lib/i386-linux-gnu \
  -Wl,-rpath-link,$D/ubu_rt/usr/lib32 \
  -Wl,-rpath-link,$D/dev/lib/i386-linux-gnu \
  -Wl,-rpath-link,$D/dev/usr/lib/i386-linux-gnu"

# shellcheck disable=SC2086
gcc $COMMON -o "$SRC/flfetch-i386" "$SRC/flfetch.c" "$TCW"
strip "$SRC/flfetch-i386" 2>/dev/null || true
echo "OK i386: $SRC/flfetch-i386"

if [ "${BUILD_HOST:-0}" = "1" ]; then
    gcc -O2 -Wall -Wextra -std=gnu11 -I/usr/include/ncursesw \
        -o "$SRC/flfetch-host" "$SRC/flfetch.c" -lncursesw
    echo "OK host: $SRC/flfetch-host"
fi
