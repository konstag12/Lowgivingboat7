#!/usr/bin/env bash
# Reproducible build from Linux:
#   * native POSIX build of core + tests + bench (runs here)
#   * Windows x64 cross build with MinGW-w64 (pcport_testscene.exe etc.)
# Full log goes to results/build_log.txt.
#
#   ./build.sh            build both
#   ./build.sh --test     build both, then run the quick test suite natively
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p results
LOG=results/build_log.txt
: > "$LOG"
log() { echo "$@" | tee -a "$LOG"; }
run() { log "\$ $*"; "$@" 2>&1 | tee -a "$LOG"; return "${PIPESTATUS[0]}"; }

log "=== pc-port build $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
log "host: $(uname -srm)"
log "cmake: $(cmake --version | head -1)"
log "native: $(g++ --version | head -1)"
log "cross:  $(x86_64-w64-mingw32-g++-posix --version | head -1)"

run cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release
run cmake --build build/linux
run cmake -S . -B build/win64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
run cmake --build build/win64

log "--- outputs ---"
for f in build/win64/*.exe build/linux/pcport_tests build/linux/pcport_bench build/linux/pcport_inventory; do
  log "$(sha256sum "$f" | cut -c1-16)  $(stat -c %s "$f") bytes  $f  [$(file -b "$f" | cut -d, -f1-2)]"
done
log "--- Windows DLL imports of pcport_testscene.exe ---"
x86_64-w64-mingw32-objdump -p build/win64/pcport_testscene.exe | grep 'DLL Name' | tee -a "$LOG"

if [[ "${1:-}" == "--test" ]]; then
  run ./build/linux/pcport_tests --quick --tmp build
fi
log "=== build OK ==="
