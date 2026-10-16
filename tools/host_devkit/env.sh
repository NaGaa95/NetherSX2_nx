# Zdroj tohle:  . tools/host_devkit/env.sh
export DEVKITPRO="${DEVKITPRO:-/home/user/devkitpro}"
export DEVKITA64="$DEVKITPRO/devkitA64"
export DEVKITARM="$DEVKITPRO/devkitARM"
export PATH="${TC_ROOT:-/home/user/toolchain}/venv/bin:$DEVKITPRO/tools/bin:$PATH"
