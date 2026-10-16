#!/usr/bin/env bash
set -euo pipefail
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export DEVKITARM=$DEVKITPRO/devkitARM
export DEVKITA64=$DEVKITPRO/devkitA64
JOBS=${JOBS:-18}
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo "JOBS must be a positive integer." >&2; exit 1; }

APP="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$APP")"
if [[ -z "${CORES_DIR:-}" ]]; then
  CORES_DIR="$ROOT"
  if [[ -d "$APP/out/cores" ]]; then CORES_DIR="$APP/out/cores"; fi
fi
if [[ -n "${APK_4248:-}${APK_3668:-}" ]]; then
  [[ -n "${APK_4248:-}" && -n "${APK_3668:-}" ]] || {
    echo "Set both APK_4248 and APK_3668." >&2; exit 1;
  }
  CORES_DIR="$APP/out/cores"
  python3 "$APP/tools/prepare_build_inputs.py" --apk-4248 "$APK_4248" \
    --apk-3668 "$APK_3668" --cores-dir "$CORES_DIR"
fi
if [[ -n "${MESA_SDK_ZIP:-}" ]]; then
  export MESA_SDK_ROOT="$APP/out/mesa-sdk"
  python3 "$APP/tools/prepare_build_inputs.py" --mesa-sdk "$MESA_SDK_ZIP" --sdk-dir "$MESA_SDK_ROOT"
fi

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
  # Mesa NVK, same two layouts the Makefile detects: the nxvk portlib package
  # (preferred) or the older vendored tree under $APP/vulkan.
  NXVK_PKG_CONFIG="$DEVKITA64/bin/aarch64-none-elf-pkg-config"
  if PKG_CONFIG_PATH= "$NXVK_PKG_CONFIG" --exists nxvk 2>/dev/null; then
    NXVK_PREFIX=$(PKG_CONFIG_PATH= "$NXVK_PKG_CONFIG" --variable=prefix nxvk)
    echo "NVK: portlib package $NXVK_PREFIX ($(PKG_CONFIG_PATH= "$NXVK_PKG_CONFIG" --modversion nxvk))"
    required+=(
      "$NXVK_PREFIX/lib/libnvk.a"
      "$NXVK_PREFIX/lib/libnvk_support.a"
      "$NXVK_PREFIX/lib/pkgconfig/nxvk.pc"
      "$NXVK_PREFIX/include/vulkan/vulkan_core.h"
    )
  else
    echo "NVK: vendored tree $APP/vulkan (nxvk.pc not found)"
    required+=(
      "$APP/vulkan/include/vulkan/vulkan_core.h"
      "$APP/vulkan/lib/libnvk.a"
    )
  fi
fi
for file in "${required[@]}"; do
  [[ -f "$file" ]] || { echo "Missing build input: $file" >&2; exit 1; }
done
command -v cmake >/dev/null || { echo "cmake is required." >&2; exit 1; }
command -v ninja >/dev/null || { echo "ninja is required." >&2; exit 1; }

DEPS_BUILD="$APP/launcher/dependencies/build"
if [[ -f "$DEPS_BUILD/CMakeCache.txt" ]] &&
   ! grep -q '^CMAKE_GENERATOR:INTERNAL=Ninja$' "$DEPS_BUILD/CMakeCache.txt"; then
  echo "Storage dependency cache uses another generator: $DEPS_BUILD" >&2
  echo "Move the old generated build directory before rebuilding with Ninja." >&2
  exit 1
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

echo "==== native Deko3D shader compiler + LSFG dependencies ===="
DEKO3D_DEPS_ROOT="${DEKO3D_DEPS_ROOT:-$APP/out/deko3d-deps}"
dk_args=(
  -S "$APP/source/deko3d/compiler"
  -B "$DEKO3D_DEPS_ROOT"
  -G Ninja
  -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake"
  -DCMAKE_BUILD_TYPE=Release
  -DDEKO3D_SHADER_TESTS=OFF
)
if [[ -n "${UAM_SOURCE:-}" ]]; then
  dk_args+=( -DFETCHCONTENT_SOURCE_DIR_DOLPHIN_UAM="$UAM_SOURCE" )
fi
if [[ -n "${SPIRV_CROSS_SOURCE:-}" ]]; then
  dk_args+=( -DFETCHCONTENT_SOURCE_DIR_SPIRV_CROSS="$SPIRV_CROSS_SOURCE" )
fi
cmake "${dk_args[@]}"
cmake --build "$DEKO3D_DEPS_ROOT" --parallel "$JOBS"

echo "==== emulator: native Deko3D ===="
cd "$APP"
DK_BUILD=build-dk
DK_TARGET=NetherSX2_nx_dk
make -j"$JOBS" RENDERER=DK BUILD="$DK_BUILD" TARGET="$DK_TARGET" DEKO3D_DEPS_ROOT="$DEKO3D_DEPS_ROOT"

echo "==== emulator: Vulkan (NVK) ===="
make -j"$JOBS" RENDERER=VK BUILD=build-vk TARGET=NetherSX2_nx_vk

echo "==== emulator: OpenGL ===="
make -j"$JOBS" RENDERER=GL BUILD=build-gl TARGET=NetherSX2_nx_gl

echo "==== bundle cores + emulator binaries into the launcher romfs ===="
mkdir -p "$APP/launcher/romfs/cores" "$APP/launcher/romfs/emu"
cp -f "$CORES_DIR/NetherSX2-v2.2n-4248/lib/arm64-v8a/libemucore.so" "$APP/launcher/romfs/cores/emucore_4248.so"
cp -f "$CORES_DIR/NetherSX2-v2.2n-3668/lib/arm64-v8a/libemucore.so" "$APP/launcher/romfs/cores/emucore_3668.so"
cp -f "$APP/NetherSX2_nx_vk.nro" "$APP/launcher/romfs/emu/NetherSX2_nx_vk.nro"
cp -f "$APP/NetherSX2_nx_gl.nro" "$APP/launcher/romfs/emu/NetherSX2_nx_gl.nro"
cp -f "$APP/$DK_TARGET.nro" "$APP/launcher/romfs/emu/NetherSX2_nx_dk.nro"

echo "==== bundle per-build resources into the launcher romfs ===="
python3 "$APP/tools/bundle_resources.py" --cores-dir "$CORES_DIR"

echo "==== forwarder stub (built in-tree from launcher/fwd/) ===="
make -j"$JOBS" -C "$APP/launcher/fwd"

echo "==== launcher (SDL2 addon) ===="
cd "$APP/launcher"
make -j"$JOBS"

mv -f "$APP/launcher/NetherSX2.nro" "$APP/NetherSX2.nro"
python3 "$APP/tools/check_deko3d_build.py" --cores-dir "$CORES_DIR" \
  --map "$APP/$DK_BUILD/$DK_TARGET.map" --native-nro "$APP/$DK_TARGET.nro" --bundle "$APP/NetherSX2.nro"

echo
echo "Done. The only file to copy:"
ls -la "$APP/NetherSX2.nro"
echo
echo "SD layout: sdmc:/switch/NetherSX2.nro + sdmc:/switch/nethersx2/"
