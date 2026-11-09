#!/bin/sh
# pgo.sh - profile-guided build of the game binary.
#
#   RERED_DEV=root@HANDHELD tools/pgo.sh [gen|train|use|all]
#
#  gen    instrumented build in build/pgo (PGO=gen), installed on the handheld
#  train  one scripted session on the handheld (needs the game imported there and
#         nothing else running): intro movies, menus, new game, the opening
#         cutscene, then walking, turning, firing and night vision; the counters
#         are written after RERED_GCOV_DUMP seconds and copied to build/pgo-data
#  use    optimised build in build/pgo (PGO=use); the binary to ship is
#         build/pgo/rered (tools/package.sh PGO=1 packages it)
#  all    gen, train, use (default)
#
# The handheld runs the dev-run.sh helper from packaging/ (copied by gen).
set -eu
cd "$(dirname "$0")/.."
DEV=${RERED_DEV:?set RERED_DEV=user@handheld}
D=${RERED_PORT_DIR:-/mnt/sdcard/ports/rered_faction2}
DOCKER="docker run --rm -u $(id -u):$(id -g) -v $PWD:/src -w /src rered-cross:bookworm"
STEP=${1:-all}

gen() {
    rm -rf build/pgo build/pgo-data
    $DOCKER make ARCH=aarch64 OUT=build/pgo PGO=gen -j"$(nproc)"
    $DOCKER aarch64-linux-gnu-strip -o build/rered.pgogen build/pgo/rered
    ssh "$DEV" "cat > $D/rered.aarch64.tmp && chmod +x $D/rered.aarch64.tmp && mv $D/rered.aarch64.tmp $D/rered.aarch64" < build/rered.pgogen
    ssh "$DEV" "cat > $D/dev-run.sh && chmod +x $D/dev-run.sh" < packaging/dev-run.sh
}

# DirectInput scan codes: W 17, A 30, D 32, N 49. Times in seconds from start.
SCRIPT='42:m 190 -136;43.5:c;50:m -46 136;51:c;53:c;56:m -141 -11;57:c;59:c;61:k 28;'\
'180:K 17 3000;184:m 300 0;185:K 17 2500;188:c;189:c;190:k 49;192:K 17 3000;196:m -400 0;'\
'197:K 30 2000;200:k 49;202:K 17 4000;207:m 0 -100;208:c;210:K 32 2000;213:m 250 40;214:K 17 3000'

train() {
    ssh "$DEV" "rm -rf $D/pgo; mkdir -p $D/pgo; cd $D && \
        GCOV_PREFIX=$D/pgo GCOV_PREFIX_STRIP=0 RERED_GCOV_DUMP=225 RERED_SCRIPT='$SCRIPT' ./dev-run.sh 245 >/dev/null 2>&1; \
        grep -a '\[pgo\] profile written' log.txt"
    rm -rf build/pgo-data; mkdir -p build/pgo-data
    ssh "$DEV" "cd $D/pgo/src/build/pgo-data && tar cf - ." | tar xf - -C build/pgo-data
    ssh "$DEV" "rm -rf $D/pgo"
    echo "profile: $(find build/pgo-data -name '*.gcda' | wc -l) files"
}

use() {
    find build/pgo -name '*.o' -delete 2>/dev/null || true
    $DOCKER make ARCH=aarch64 OUT=build/pgo PGO=use -j"$(nproc)"
    echo "build/pgo/rered: profile-guided build"
}

case "$STEP" in
    gen) gen ;; train) train ;; use) use ;;
    all) gen; train; use ;;
    *) echo "usage: tools/pgo.sh [gen|train|use|all]" >&2; exit 2 ;;
esac
