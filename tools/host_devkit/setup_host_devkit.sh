#!/usr/bin/env bash
# Host-only devkitPro sysroot pro NetherSX2_nx. Idempotentni, ~10 s.
#
# Sandbox muze jen: github.com, codeload.github.com, api.github.com, pypi.org,
# files.pythonhosted.org, registry.npmjs.org.
# BLOKOVANO (overeno): pkg.devkitpro.org, apt.devkitpro.org,
# downloads.devkitpro.org, deb.debian.org, release-assets.githubusercontent.com,
# registry-1.docker.io, ghcr.io.  => zadny kompilator, zadne portlibs, zadny image.
#
# Vysledek: cmake/ninja/mako + hlavicky libnx & deko3d + switch-tools postavene
# ze zdroje. Staci na CMake konfiguraci a host testy. NESTACI na link .nro.
set -euo pipefail

DK="${DEVKITPRO:-/home/user/devkitpro}"
TC="${TC_ROOT:-/home/user/toolchain}"
LIBNX_REF="${LIBNX_REF:-master}"
DEKO3D_REF="${DEKO3D_REF:-master}"
SWTOOLS_REF="${SWTOOLS_REF:-master}"

echo "==== 1/5  cmake + ninja + mako (PyPI) ===="
if [[ ! -x "$TC/venv/bin/cmake" ]]; then
  python3 -m venv "$TC/venv"
  "$TC/venv/bin/pip" install --quiet --upgrade pip
  "$TC/venv/bin/pip" install --quiet cmake ninja mako
fi
"$TC/venv/bin/cmake" --version | head -1
"$TC/venv/bin/ninja" --version
"$TC/venv/bin/python" -c 'import mako; print("mako", mako.__version__)'
for t in cmake ctest cpack ninja; do sudo ln -sf "$TC/venv/bin/$t" "/usr/local/bin/$t" 2>/dev/null || true; done

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
fetch() { # fetch <owner/repo> <ref> <dest>
  curl -sSL --retry 3 -o "$tmp/$3.tgz" "https://codeload.github.com/$1/tar.gz/refs/heads/$2" \
    || curl -sSL --retry 3 -o "$tmp/$3.tgz" "https://codeload.github.com/$1/tar.gz/$2"
  mkdir -p "$tmp/$3" && tar xzf "$tmp/$3.tgz" -C "$tmp/$3" --strip-components=1
}

echo "==== 2/5  libnx hlavicky + rules (switchbrew/libnx@$LIBNX_REF) ===="
fetch switchbrew/libnx "$LIBNX_REF" libnx
mkdir -p "$DK/libnx/include" "$DK/libnx/lib"
cp -r "$tmp/libnx/nx/include/." "$DK/libnx/include/"
cp "$tmp/libnx/nx/switch_rules" "$tmp/libnx/nx/switch.specs" "$tmp/libnx/nx/switch.ld" "$DK/libnx/"

echo "==== 3/5  deko3d hlavicky (devkitPro/deko3d@$DEKO3D_REF) ===="
fetch devkitPro/deko3d "$DEKO3D_REF" deko3d
mkdir -p "$DK/portlibs/switch/include" "$DK/portlibs/switch/lib/pkgconfig"
cp "$tmp/deko3d/include/deko3d.h" "$tmp/deko3d/include/deko3d.hpp" "$DK/portlibs/switch/include/"
cp "$tmp/deko3d/include/deko3d.h" "$tmp/deko3d/include/deko3d.hpp" "$DK/libnx/include/"

# switch_rules vola elf2nro / elf2nso / elf2kip / nacptool / npdmtool / build_pfs0.
# Tohle jsou HOST nastroje, daji se postavit host gcc. elf2nro potrebuje lz4,
# jehoz prebuilt verze je release asset (blokovano) -> stavime liblz4.a ze zdroje.
echo "==== 4/5  switch-tools ze zdroje (elf2nro, nacptool, npdmtool, ...) ===="
mkdir -p "$DK/tools/bin"
if [[ ! -x "$DK/tools/bin/elf2nro" ]]; then
  if fetch lz4/lz4 "${LZ4_REF:-dev}" lz4 && fetch switchbrew/switch-tools "$SWTOOLS_REF" swtools; then
    ( cd "$tmp/lz4/lib" && for f in lz4.c lz4hc.c lz4frame.c xxhash.c; do
        gcc -O2 -I. -c "$f" -o "${f%.c}.o" || exit 1; done && ar rcs liblz4.a ./*.o )
    LZ="$tmp/lz4/lib"
    # helper soubory bez main(); nxlink.c a build_romfs.c maji vlastni main
    HELPERS="cJSON.c filepath.c romfs.c sha256.c blz.c"
    for t in elf2nro elf2nso elf2kip nacptool npdmtool build_pfs0 build_romfs; do
      gcc -O2 -std=gnu99 -w -I"$tmp/swtools/src" -I"$LZ" \
          "$tmp/swtools/src/$t.c" \
          $(for h in $HELPERS; do echo "$tmp/swtools/src/$h"; done) \
          "$LZ/liblz4.a" -o "$DK/tools/bin/$t" 2>"$tmp/$t.err" \
        && echo "  postaveno: $t" \
        || echo "  SELHALO:   $t  ($(head -1 "$tmp/$t.err" | cut -c1-70))"
    done
    # bin2s je v devkitPro/general-tools a zada PACKAGE_STRING z autotools
    if fetch devkitPro/general-tools "${GENTOOLS_REF:-master}" gentools; then
      gcc -O2 -w -DPACKAGE_STRING='"bin2s"' "$tmp/gentools/bin2s.c" \
          -o "$DK/tools/bin/bin2s" && echo "  postaveno: bin2s" \
        || echo "  SELHALO:   bin2s"
    fi
  else
    echo "  switch-tools zdroje nedostupne, preskakuji"
  fi
fi

echo "==== 5/5  HOST STUB EGL/GLES2 (jen pro tests/patches) ===="
mkdir -p "$DK/portlibs/switch/include/EGL" "$DK/portlibs/switch/include/GLES2"
cat > "$DK/portlibs/switch/include/EGL/egl.h" <<'EOF'
/* HOST-TEST STUB -- NOT the real Khronos/Mesa EGL header.
 * Jen typedefy, ktere odkazuje source/hooks.h, aby tests/patches slo zkompilovat
 * host kompilatorem. Pro opravdovy Switch build patri switch-mesa, ne tohle. */
#ifndef NETHERSX2_HOST_STUB_EGL_H
#define NETHERSX2_HOST_STUB_EGL_H
#include <stdint.h>
typedef int32_t EGLint;
typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;
typedef unsigned int EGLuint;
typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef void *EGLNativeDisplayType;
typedef void *EGLNativeWindowType;
typedef void *EGLNativePixmapType;
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_DEFAULT_DISPLAY ((EGLNativeDisplayType)0)
#endif
EOF
cat > "$DK/portlibs/switch/include/GLES2/gl2.h" <<'EOF'
/* HOST-TEST STUB -- NOT the real Khronos GLES2 header. Viz EGL/egl.h. */
#ifndef NETHERSX2_HOST_STUB_GL2_H
#define NETHERSX2_HOST_STUB_GL2_H
typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef signed char GLbyte;
typedef short GLshort;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef unsigned int GLuint;
typedef float GLfloat;
typedef float GLclampf;
typedef void GLvoid;
typedef char GLchar;
#endif
EOF

mkdir -p "$DK/devkitA64/bin" "$DK/cmake"
echo
echo "Sysroot: $DK"
echo "  libnx hlavicky : $(find "$DK/libnx/include" -name '*.h' | wc -l)"
echo "  tools/bin      : $(ls "$DK/tools/bin" 2>/dev/null | tr '\n' ' ')"
echo "  libnx/lib      : $(find "$DK/libnx/lib" -type f | wc -l) souboru (CEKANE 0 -- neni cross kompilator)"
echo "  devkitA64/bin  : $(find "$DK/devkitA64/bin" -type f | wc -l) souboru (CEKANE 0 -- blokovano)"
