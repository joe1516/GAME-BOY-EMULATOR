#!/usr/bin/env bash
# Downloads freely distributed test ROMs (Blargg's cpu_instrs/instr_timing,
# Matt Currie's dmg-acid2) into roms/tests/ and runs them with gbcli.
# Needs: bash, curl, and a built ./gbcli  (run `make` first).
set -u
cd "$(dirname "$0")/.."
mkdir -p roms/tests
base=https://raw.githubusercontent.com/retrio/gb-test-roms/master

fetch() { [ -f "roms/tests/$1" ] || curl -fsSL -o "roms/tests/$1" "$2" || echo "could not download $1"; }

fetch cpu_instrs.gb   "$base/cpu_instrs/cpu_instrs.gb"
fetch instr_timing.gb "$base/instr_timing/instr_timing.gb"
fetch dmg-acid2.gb    "https://github.com/mattcurrie/dmg-acid2/releases/download/v1.0/dmg-acid2.gb"

fail=0
for rom in cpu_instrs instr_timing; do
    printf '%-14s ' "$rom"
    if ./gbcli "roms/tests/$rom.gb" --serial --frames 8000 2>&1 | grep -q Passed; then
        echo PASS
    else
        echo FAIL; fail=1
    fi
done

./gbcli roms/tests/dmg-acid2.gb --frames 120 --screenshot roms/tests/dmg-acid2-output.ppm 2>/dev/null
echo "dmg-acid2      screenshot saved to roms/tests/dmg-acid2-output.ppm (compare with the reference image)"
exit $fail
