#!/bin/bash
# compare_bins.sh OLD_BIN NEW_BIN OUTDIR: run the same headless cases through both binaries
# (the game's data in work/data) and report the screenshots and logs that differ.
OLD="$1"; NEW="$2"; OUT="$3"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DATA="$ROOT/work/data"
cd "$ROOT" || exit 1
mkdir -p "$OUT"
SHIP=(--level levels/Ship_01.lvc --spawn-at 1.5,-247,3.9,180 --exec "Enable(17314) Enable(17315) Enable(17316) EnableShootControl(1)" --god)
declare -A CASES
CASES[sf1]='--skip-intro --run 3'
CASES[mainmenu]='--main-menu d.e'
CASES[pause]='--skip-intro --menu dde'
CASES[loading]='--loading-screen 1'
CASES[death]='--skip-intro --death-screen 1.0'
CASES[endscreen]='--skip-intro --end-screen 2'
CASES[walk]='--skip-intro --walk-test 3'
CASES[touch]='--skip-intro --touch-test 1 --run 2'
CASES[volteron]='--level levels/13_volteron_1.lvc --skip-intro --run 2.5'
CASES[lava]='--level levels/09_volteron_3.lvc --section 3 --no-scripts --eye 250,420,-95,0,-20 --time 2'
fail=0
run_case() {   # name, then args
    local name="$1"; shift
    for which in old new; do
        local bin="$OLD"; [ "$which" = new ] && bin="$NEW"
        "$bin" --data "$DATA" --mute --size 640x360 --screenshot "$OUT/$name.$which.bmp" "$@" 2>&1 |
            grep -v "loaded in\|compiled\|bench:\|from cache\|^wrote " > "$OUT/$name.$which.log"
    done
    local ae
    ae=$(magick compare -metric AE "$OUT/$name.old.bmp" "$OUT/$name.new.bmp" null: 2>&1 | cut -d' ' -f1)
    if ! cmp -s "$OUT/$name.old.log" "$OUT/$name.new.log"; then
        echo "$name: LOG DIFFERS (image AE $ae)"; diff "$OUT/$name.old.log" "$OUT/$name.new.log" | head -5; fail=1
    elif [ "$ae" != "0" ]; then
        echo "$name: IMAGE DIFFERS (AE $ae)"; fail=1
    else
        echo "$name: same"
    fi
}
for name in "${!CASES[@]}"; do
    # shellcheck disable=SC2086
    run_case "$name" ${CASES[$name]}
done
run_case ship_combat "${SHIP[@]}" --combat-test 3
run_case ship_grenade "${SHIP[@]}" --grenade-test 0.5 --run 3
run_case sound --skip-intro --run 3 --sound-render "$OUT/sound.wav"
exit $fail
