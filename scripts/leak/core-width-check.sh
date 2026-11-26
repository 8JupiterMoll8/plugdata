#!/usr/bin/env bash
# Headless libpd check for the comment-width heap corruption (see core-width-check.c).
#   core-width-check.sh [libpd-src.a] [pure-data/src dir]
# Defaults: the build clone's single-instance Pd core (the one the standalone links) and its source headers.
# libpd-src.a leaves the externals' *_setup() symbols undefined (they live in libexternals.a, which drags in ffmpeg,
# Gem, luajit...). None is called by this test, so the link step stubs whatever the first link reports undefined.
set -uo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
clone="${PLUGDATA_BUILD_CLONE:-$HOME/Desktop/plugdata/plugdata-p31-build}"
lib="${1:-$clone/build/Libraries/libpd-src.a}"
src="${2:-$clone/Libraries/pure-data/src}"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
defs="-DPD=1 -DPD_INTERNAL=1 -DUSEAPI_DUMMY=1 -DLIBPD_EXTRA=1 -DHAVE_LIBDL=1 -DHAVE_UNISTD_H=1 -DHAVE_ALLOCA_H=1 -DHAVE_ALLOCA=1 -DDONT_USE_ALLOCA=0"
cc="${CC:-cc}"
$cc -std=gnu11 -O1 -g $defs -w -I"$src" -c "$here/core-width-check.c" -o "$out/t.o" || exit 2
echo 'void __stub_marker(void){}' > "$out/stubs.c"
for pass in 1 2 3; do
  $cc -w -c "$out/stubs.c" -o "$out/stubs.o"
  if $cc -o "$out/t" "$out/t.o" "$out/stubs.o" "$lib" -lm -lpthread -ldl -Wl,--allow-multiple-definition 2> "$out/link.err"; then break; fi
  grep -o "undefined reference to \`[A-Za-z0-9_]*'" "$out/link.err" | sed "s/.*\`//; s/'//" | sort -u |
    awk '{printf "void %s(void){}\n", $1}' >> "$out/stubs.c"
done
[ -x "$out/t" ] || { cat "$out/link.err" | head -20; echo "core-width-check: link failed" >&2; exit 2; }
MALLOC_CHECK_=3 MALLOC_PERTURB_=165 "$out/t" "$out"
