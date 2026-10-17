#!/usr/bin/env bash
# audit_build_requirements.sh -- co vyzaduje ./build_all.sh a mame to?
#
# Jen cte, nic nestavi. Exit code nenulovy, dokud neco chybi.
#   usage: bash tools/host_devkit/audit_build_requirements.sh [repo]
#
# Cesty se parsuji z Makefile / launcher Makefile / build_all.sh / CMakeLists,
# takze audit nezastara, kdyz se build zmeni.
set -uo pipefail

REPO="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
DK="${DEVKITPRO:-/home/user/devkitpro}"
PL="$DK/portlibs/switch"
LNX="$DK/libnx"
HAVE=0; MISS=0; NETOK=0; NETBAD=0
G="\033[32m"; R="\033[31m"; Y="\033[33m"; C="\033[36m"; N="\033[0m"
ok()  { printf "  ${G}HAVE${N}  %-44s %s\n" "$1" "${2:-}"; HAVE=$((HAVE+1)); }
no()  { printf "  ${R}MISS${N}  %-44s %s\n" "$1" "${2:-}"; MISS=$((MISS+1)); }
net() { if [[ "$2" == 200 ]]; then printf "  ${G}NET${N}   %-44s %s\n" "$1" "$2"; NETOK=$((NETOK+1));
        else printf "  ${R}BLOCK${N} %-44s %s\n" "$1" "$2"; NETBAD=$((NETBAD+1)); fi; }
hdr() { printf "\n${C}== %s ==${N}\n" "$1"; }
f()   { [[ -e "$2" ]] && ok "$1" "$2" || no "$1" "chybi: $2"; }
h()   { [[ -e "$2" ]] && ok "$1" "$2" || no "$1" "chybi hlavicka: $2"; }
lib() { local x; for x in "$2/lib$3.a" "$2/lib$3.so"; do [[ -e "$x" ]] && { ok "$1" "$x"; return; }; done
        no "$1" "chybi: $2/lib$3.{a,so}"; }
b()   { local p; p=$(command -v "$2" 2>/dev/null); [[ -n "$p" ]] && ok "$1" "$p" || no "$1" "v PATH neni: $2"; }

printf "${C}Audit build pozadavku%s\n" " -- $REPO"
printf "DEVKITPRO=%s\n" "$DK"
[[ -d "$REPO" ]] || { echo "repo nenalezeno" >&2; exit 2; }

hdr "1. Hostovske nastroje (README: CMake, Ninja, Git, Bison, Flex, Python, Mako)"
b cmake cmake; b ninja ninja; b make make; b git git; b python3 python3
b bison bison; b flex flex; b "pkg-config (host)" pkg-config
if python3 -c 'import mako.template' 2>/dev/null; then
  ok "python Mako" "$(python3 -c 'import mako;print(mako.__version__)' 2>/dev/null)"
else no "python Mako" "pip install mako"; fi

hdr "2. devkitA64 -- cross kompilator (BALICKOVANY, NENI V GITU)"
f "devkitA64/base_rules (switch_rules:10)" "$DK/devkitA64/base_rules"
for t in gcc g++ ar as ld objcopy strip ranlib nm; do
  f "devkitA64/bin/aarch64-none-elf-$t" "$DK/devkitA64/bin/aarch64-none-elf-$t"; done
f "aarch64-none-elf sysroot (newlib)" "$DK/devkitA64/aarch64-none-elf"

hdr "3. libnx"
f "libnx/switch_rules"     "$LNX/switch_rules"
f "libnx/switch.specs"     "$LNX/switch.specs"
f "libnx/switch.ld"        "$LNX/switch.ld"
f "libnx/default_icon.jpg" "$LNX/default_icon.jpg"
h "libnx/include/switch.h" "$LNX/include/switch.h"
lib "libnx/lib/libnx.a  (-lnx)" "$LNX/lib" nx

hdr "4. switch-tools -- switch_rules %.nro: %.elf"
for t in elf2nro elf2nso elf2kip nacptool npdmtool build_pfs0 bin2s; do
  f "tools/bin/$t" "$DK/tools/bin/$t"; done

hdr "5. devkitpro-cmake -- build_all.sh: -DCMAKE_TOOLCHAIN_FILE"
f "cmake/Switch.cmake" "$DK/cmake/Switch.cmake"

hdr "6. portlibs/switch -- hlavicky"
h "deko3d.h (RENDERER=DK)"          "$PL/include/deko3d.h"
h "curl/curl.h (-lcurl)"            "$PL/include/curl/curl.h"
h "zlib.h (-lz)"                    "$PL/include/zlib.h"
h "zstd.h (-lzstd)"                 "$PL/include/zstd.h"
h "png.h (DK: -lpng)"               "$PL/include/png.h"
h "turbojpeg.h (launcher)"          "$PL/include/turbojpeg.h"
h "SDL2/SDL.h (launcher)"           "$PL/include/SDL2/SDL.h"
h "SDL2/SDL_ttf.h"                  "$PL/include/SDL2/SDL_ttf.h"
h "SDL2/SDL_image.h"                "$PL/include/SDL2/SDL_image.h"
h "EGL/egl.h (GL/VK renderer)"      "$PL/include/EGL/egl.h"
h "GLES2/gl2.h"                     "$PL/include/GLES2/gl2.h"
h "ntfs-3g/volume.h (CMake FATAL)"  "$PL/include/ntfs-3g/volume.h"
h "libelf.h (unified Mesa: -lelf)"  "$PL/include/libelf.h"
h "expat.h (unified Mesa: -lexpat)" "$PL/include/expat.h"

hdr "7. portlibs/switch -- knihovny (z LIBS v Makefile)"
lib "libdeko3d.a   (DK)"         "$PL/lib" deko3d
lib "libcurl.a     (vse)"        "$PL/lib" curl
lib "libz.a        (vse)"        "$PL/lib" z
lib "libzstd.a     (VK)"         "$PL/lib" zstd
lib "libpng.a      (DK)"         "$PL/lib" png
lib "libturbojpeg.a (launcher)"  "$PL/lib" turbojpeg
lib "libSDL2.a     (launcher)"   "$PL/lib" SDL2
lib "libSDL2_ttf.a"              "$PL/lib" SDL2_ttf
lib "libSDL2_image.a"            "$PL/lib" SDL2_image
lib "libEGL.a      (GL/VK)"      "$PL/lib" EGL
lib "libGLESv2.a   (GL/VK)"      "$PL/lib" GLESv2
lib "libglapi.a    (GL)"         "$PL/lib" glapi
lib "libdrm_nouveau.a (GL)"      "$PL/lib" drm_nouveau
lib "libntfs-3g.a (STORAGE_LIBS)" "$PL/lib" ntfs-3g

hdr "8. pkg-config .pc (launcher Makefile: PKGS := sdl2 SDL2_ttf SDL2_image libcurl)"
for pc in sdl2 SDL2_ttf SDL2_image libcurl; do f "$pc.pc" "$PL/lib/pkgconfig/$pc.pc"; done

hdr "9. Vulkan / Mesa NVK -- NUTNE DODAT (Makefile: LIBDIRS := \$(TOPDIR)/vulkan)"
h "vulkan/include/vulkan/vulkan_core.h" "$REPO/vulkan/include/vulkan/vulkan_core.h"
lib "vulkan/lib/libnvk.a" "$REPO/vulkan/lib" nvk
mapfile -t NVK < <(sed -n '/RENDERER),VK/,/^endif/p' "$REPO/Makefile" \
  | sed 's/#.*//' | grep -oE '\-l:lib[A-Za-z0-9_]+\.a' \
  | sed 's/-l:lib//; s/\.a$//' | grep -vx 'X' | sort -u)
found=0; for a in "${NVK[@]}"; do [[ -e "$REPO/vulkan/lib/lib$a.a" ]] && found=$((found+1)); done
printf "  Makefile zada %d archivu z vulkan/lib\n" "${#NVK[@]}"
if [[ "${#NVK[@]}" -gt 0 && "$found" -eq "${#NVK[@]}" ]]; then
  ok "vulkan/lib komplet" "$found/${#NVK[@]} archivu"
else no "vulkan/lib komplet" "nalezeno $found/${#NVK[@]} archivu"; fi

hdr "10. Alternativni unified Mesa SDK (MESA_SDK_ROOT v build_all.sh)"
if [[ -n "${MESA_SDK_ROOT:-}" ]]; then
  for x in include/vulkan/vulkan_core.h include/EGL/egl.h lib/libvulkan.a lib/libEGL.a; do
    f "MESA_SDK_ROOT/$x" "$MESA_SDK_ROOT/$x"; done
else printf "  ${Y}----${N}  %-44s %s\n" "MESA_SDK_ROOT" "nenastaveno -> pouzije se vulkan/ (bod 9)"; fi

hdr "11. APK cores -- NUTNE DODAT (build_all.sh: required[])"
for v in 4248 3668; do
  d="${CORES_DIR:-$(dirname "$REPO")}/NetherSX2-v2.2n-$v"
  [[ -d "$REPO/out/cores/NetherSX2-v2.2n-$v" ]] && d="$REPO/out/cores/NetherSX2-v2.2n-$v"
  f "core $v: libemucore.so"          "$d/lib/arm64-v8a/libemucore.so"
  f "core $v: assets/GameIndex.yaml"  "$d/assets/GameIndex.yaml"
done

hdr "12. Deko3D deps (build_all.sh krok 2 -> DEKO3D_DEPS_ROOT)"
DDR="${DEKO3D_DEPS_ROOT:-$REPO/out/deko3d-deps}"
lib "liblsfg.a"             "$DDR/lsfg" lsfg
lib "liblsfg_spirv_cross.a" "$DDR/lsfg" lsfg_spirv_cross
f "uam-generated/uam_isolated.o" "$DDR/uam-generated/uam_isolated.o"
printf "  ${Y}pozn${N}  stavi se z gitu, ale vyzaduje bison+flex a cross kompilator (body 1, 2)\n"

hdr "13. FetchContent zdroje (pinnute revize, build_all.sh je stahuje z GitHubu)"
probe() { curl -sS -m 20 -o /dev/null -w "%{http_code}" "https://api.github.com/repos/$1/commits/$2"; }
net "ITotalJustice/libsmb2 @867beea"    "$(probe ITotalJustice/libsmb2 867beea093f2863dfddea01945204f724afd6c45)"
net "ITotalJustice/libusbhsfs @625269b" "$(probe ITotalJustice/libusbhsfs 625269b7725a6e2a3f2724e8d45b602c1b20ead5)"
net "PalindromicBreadLoaf/uam @c0d76aa" "$(probe PalindromicBreadLoaf/uam c0d76aa9107d67edcd35a1b717ace87ee7939c68)"
net "KhronosGroup/SPIRV-Cross @ebe2aa0" "$(probe KhronosGroup/SPIRV-Cross ebe2aa0cd80f5eb5cd8a605da604cacf72205f3b)"

hdr "14. Hosty mimo allowlist sandboxu"
for u in https://pkg.devkitpro.org https://apt.devkitpro.org https://downloads.devkitpro.org \
         https://deb.debian.org https://release-assets.githubusercontent.com \
         https://registry-1.docker.io/v2/ https://ghcr.io/v2/; do
  net "$u" "$(curl -sS -m 10 -o /dev/null -w '%{http_code}' "$u" 2>/dev/null | head -c3)"
done

printf "\n${C}== SOUHRN ==${N}\n"
printf "  splneno: ${G}%d${N}   chybi: ${R}%d${N}   sit ok: ${G}%d${N}   sit blokovano: ${R}%d${N}\n" \
  "$HAVE" "$MISS" "$NETOK" "$NETBAD"
[[ "$MISS" -eq 0 ]]
