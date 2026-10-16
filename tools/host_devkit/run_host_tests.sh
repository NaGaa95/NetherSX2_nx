#!/usr/bin/env bash
# Postav a spust kazdy validacni test NetherSX2_nx, ktery bezi na HOSTITELI.
#
# 7 z 8: tests/patches a tests/network pres CMake+ctest; tests/deko3d
#   (transfer, render_target, vertex_format, render_state, presentation) primo
#   g++, protoze source/deko3d/compiler/CMakeLists.txt includuje Uam.cmake
#   bezpodminene a to bez bison+flex spadne na FATAL_ERROR.
# 1 z 8 preskocen: tests/deko3d/shader_validation -- linkuje `uam`, vyzaduje
#   bison + flex. build_all.sh ho stejne vypina (-DDEKO3D_SHADER_TESTS=OFF).
set -uo pipefail

REPO="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
DK="${DEVKITPRO:-/home/user/devkitpro}"
WORK="${HOST_TEST_BUILD:-/tmp/nethersx2-host-tests}"
PASS=0; FAIL=0
report() { if [[ "$2" -eq 0 ]]; then printf '  \033[32mPASS\033[0m  %-32s %s\n' "$1" "${3:-}"; PASS=$((PASS+1));
           else printf '  \033[31mFAIL\033[0m  %-32s %s\n' "$1" "${3:-}"; FAIL=$((FAIL+1)); fi; }

[[ -d "$REPO" ]] || { echo "repo nenalezeno: $REPO" >&2; exit 2; }
[[ -f "$DK/libnx/include/switch.h" ]] || { echo "chybi sysroot: spust setup_host_devkit.sh" >&2; exit 2; }
command -v cmake >/dev/null || PATH="/home/user/toolchain/venv/bin:$PATH"
rm -rf "$WORK"; mkdir -p "$WORK"

echo "== CMake/ctest =="
for suite in patches network; do
  ( cd "$WORK" && cmake -S "$REPO/tests/$suite" -B "$suite" -G Ninja -DCMAKE_BUILD_TYPE=Release >"$suite.log" 2>&1 \
    && cmake --build "$suite" >>"$suite.log" 2>&1 \
    && ctest --test-dir "$suite" --output-on-failure >>"$suite.log" 2>&1 ); rc=$?
  s=$(grep -oE '[0-9]+ [a-z ]*checks passed|[0-9]+% tests passed' "$WORK/$suite.log" 2>/dev/null | tail -1)
  [[ -z "$s" ]] && s=$(tail -2 "$WORK/$suite.log" 2>/dev/null | tr '\n' ' ' | cut -c1-64)
  report "$suite" "$rc" "$s"
done

echo "== Deko3D native-API validace (host g++) =="
for t in transfer render_target vertex_format render_state presentation; do
  src=("$REPO/tests/deko3d/${t}_validation.cpp")
  [[ "$t" == presentation ]] && src+=("$REPO/source/deko3d/dk_presentation.cpp")
  if ! g++ -std=c++20 -O1 -I"$REPO/source/deko3d" -I"$DK/libnx/include" "${src[@]}" \
        -o "$WORK/dk_$t" 2>"$WORK/dk_$t.err"; then
    report "deko3d/$t" 1 "$(head -2 "$WORK/dk_$t.err" | tr '\n' ' ' | cut -c1-64)"; continue
  fi
  out=$("$WORK/dk_$t" 2>&1); report "deko3d/$t" $? "$(echo "$out" | tail -1 | cut -c1-64)"
done

echo "== Preskoceno =="
printf '  \033[33mSKIP\033[0m  %-32s %s\n' "deko3d/shader_validation" "vyzaduje bison+flex pro UAM"
echo
echo "proslo=$PASS selhalo=$FAIL   logy: $WORK"
[[ "$FAIL" -eq 0 ]]
