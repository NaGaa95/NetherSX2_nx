#!/usr/bin/env bash
set -euo pipefail
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export DEVKITARM=$DEVKITPRO/devkitARM
export DEVKITA64=$DEVKITPRO/devkitA64
JOBS=${JOBS:-18}
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo "JOBS must be a positive integer." >&2; exit 1; }

APP="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$APP")"
CORES_DIR="${CORES_DIR:-$ROOT}"

required=(
  "$CORES_DIR/NetherSX2-v2.2n-4248/lib/arm64-v8a/libemucore.so"
  "$CORES_DIR/NetherSX2-v2.2n-4248/assets/GameIndex.yaml"
  "$CORES_DIR/NetherSX2-v2.2n-3668/lib/arm64-v8a/libemucore.so"
  "$CORES_DIR/NetherSX2-v2.2n-3668/assets/GameIndex.yaml"
)
if [[ -n "${MESA_SDK_ROOT:-}" ]]; then
  required+=(
    "$MESA_SDK_ROOT/include/vulkan/vulkan_core.h"
    "$MESA_SDK_ROOT/include/EGL/egl.h"
    "$MESA_SDK_ROOT/lib/libvulkan.a"
    "$MESA_SDK_ROOT/lib/libEGL.a"
  )
else
  required+=(
    "$APP/vulkan/include/vulkan/vulkan_core.h"
    "$APP/vulkan/lib/libnvk.a"
  )
fi
for file in "${required[@]}"; do
  [[ -f "$file" ]] || { echo "Missing build input: $file" >&2; exit 1; }
done
command -v cmake >/dev/null || { echo "cmake is required." >&2; exit 1; }
command -v ninja >/dev/null || { echo "ninja is required." >&2; exit 1; }

DEPS_BUILD="$APP/launcher/dependencies/build"
if [[ -f "$DEPS_BUILD/CMakeCache.txt" ]] &&
   ! grep -q '^CMAKE_GENERATOR:INTERNAL=Ninja$' "$DEPS_BUILD/CMakeCache.txt"; then
  cmake -E rm -rf "$DEPS_BUILD"
fi

echo "==== launcher storage dependencies ===="
deps_args=(
  -S "$APP/launcher/dependencies"
  -B "$DEPS_BUILD"
  -G Ninja
  -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake"
  -DCMAKE_BUILD_TYPE=Release
)
if [[ -n "${LIBSMB2_SOURCE:-}" ]]; then
  deps_args+=( -DFETCHCONTENT_SOURCE_DIR_LIBSMB2="$LIBSMB2_SOURCE" )
fi
if [[ -n "${LIBUSBHSFS_SOURCE:-}" ]]; then
  deps_args+=( -DFETCHCONTENT_SOURCE_DIR_LIBUSBHSFS="$LIBUSBHSFS_SOURCE" )
fi
cmake "${deps_args[@]}"
cmake --build "$DEPS_BUILD" --parallel "$JOBS"

echo "==== emulator: Vulkan (NVK) ===="
cd "$APP"
make clean >/dev/null 2>&1
make -j"$JOBS" RENDERER=VK
cp -f NetherSX2_nx.nro NetherSX2_nx_vk.nro

echo "==== emulator: OpenGL ===="
make clean >/dev/null 2>&1
make -j"$JOBS"
cp -f NetherSX2_nx.nro NetherSX2_nx_gl.nro

echo "==== bundle cores + emulator binaries into the launcher romfs ===="
mkdir -p "$APP/launcher/romfs/cores" "$APP/launcher/romfs/emu"
cp -f "$CORES_DIR/NetherSX2-v2.2n-4248/lib/arm64-v8a/libemucore.so" "$APP/launcher/romfs/cores/emucore_4248.so"
cp -f "$CORES_DIR/NetherSX2-v2.2n-3668/lib/arm64-v8a/libemucore.so" "$APP/launcher/romfs/cores/emucore_3668.so"
cp -f "$APP/NetherSX2_nx_vk.nro" "$APP/launcher/romfs/emu/NetherSX2_nx_vk.nro"
cp -f "$APP/NetherSX2_nx_gl.nro" "$APP/launcher/romfs/emu/NetherSX2_nx_gl.nro"

echo "==== bundle per-build resources into the launcher romfs ===="
for b in 4248 3668; do
  rd="$APP/launcher/romfs/res/$b"
  rm -rf "$rd"; mkdir -p "$rd"
  cp -rf "$CORES_DIR/NetherSX2-v2.2n-$b/assets/." "$rd/"
  rm -rf "$rd/dexopt"
done

echo "==== forwarder stub (built in-tree from launcher/fwd/) ===="
make -C "$APP/launcher/fwd" clean >/dev/null 2>&1
make -j"$JOBS" -C "$APP/launcher/fwd"

echo "==== launcher (SDL2 addon) ===="
cd "$APP/launcher"
make clean >/dev/null 2>&1
make -j"$JOBS"

mv -f "$APP/launcher/NetherSX2.nro" "$APP/NetherSX2.nro"
rm -f "$APP/NetherSX2_nx.nro" "$APP/NetherSX2_nx_vk.nro" "$APP/NetherSX2_nx_gl.nro"

echo
echo "Done. The only file to copy:"
ls -la "$APP/NetherSX2.nro"
echo
echo "SD layout: sdmc:/switch/NetherSX2.nro + sdmc:/switch/nethersx2/"
