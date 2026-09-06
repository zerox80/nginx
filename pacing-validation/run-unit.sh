#!/usr/bin/env bash
set -euo pipefail
src=$(realpath -- "${1:?configured nginx source checkout}")
build=${2:-objs}
out=${3:-$(mktemp -d)}
mkdir -p -- "$out"
out=$(realpath -- "$out")
here=$(cd -- "$(dirname -- "$0")" && pwd)
cc=${CC:-clang}
flags=(-O1 -g -Wall -Wextra -Werror -Wno-unused-parameter
       -fsanitize=address,undefined -fno-omit-frame-pointer
       -ffunction-sections -fdata-sections -Wl,--gc-sections
       -I "$src/src/core" -I "$src/src/event" -I "$src/src/event/quic"
       -I "$src/src/os/unix" -I "$src/$build")
if [[ -n ${OPENSSL_PREFIX:-} ]]; then
    flags+=(-I "$OPENSSL_PREFIX/include")
fi
for name in timer-unit pacing-unit; do
    "$cc" "${flags[@]}" "$here/$name.c" "$src/src/core/ngx_rbtree.c" \
        -o "$out/$name"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$out/$name"
done
printf 'Harness binaries retained in %s\n' "$out"
