#!/bin/bash
# Build musl cross-compiler toolchain for fully static smolmux binaries.
# Uses richfelker/musl-cross-make to build GCC 14.2.0 + musl 1.2.5 + binutils 2.44.
#
# smolmux has zero external library deps (only vendored cJSON), so this script
# only builds the cross-compiler — no zlib/openssl/libevent/curl.
#
# Output: deps/musl-toolchain-$ARCH/  (musl cross-compiler)
#
# Usage: ./scripts/build_musl_toolchain.sh [ARCH]
#   ARCH = x86_64 (default), aarch64, armv7l
#
# First build per architecture takes ~15-30 min (building GCC from source).
# Subsequent runs are cached — only needs to build once per architecture.
# The results are not checked into git.

set -euo pipefail

# -- Target architecture ------------------------------------------------------

ARCH="${1:-$(uname -m)}"

case "${ARCH}" in
    x86_64)
        MUSL_TRIPLE="x86_64-linux-musl"
        ;;
    aarch64)
        MUSL_TRIPLE="aarch64-linux-musl"
        ;;
    armv7l)
        MUSL_TRIPLE="armv7l-linux-musleabihf"
        ;;
    *)
        echo "Error: unsupported architecture '${ARCH}'" >&2
        echo "Supported: x86_64, aarch64, armv7l" >&2
        exit 1
        ;;
esac

MUSL_CC="${MUSL_TRIPLE}-gcc"

echo "=== Building musl toolchain for ${ARCH} (${MUSL_TRIPLE}) ==="

# -- Paths (arch-suffixed) ----------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
TOOLCHAIN_DIR="${PROJECT_DIR}/deps/musl-toolchain-${ARCH}"
NPROC="$(nproc)"

# -- Idempotency check --------------------------------------------------------

CC_PATH="${TOOLCHAIN_DIR}/bin/${MUSL_CC}"

if [ -x "${CC_PATH}" ]; then
    echo "Toolchain already built: ${CC_PATH}"
    echo "Delete ${TOOLCHAIN_DIR}/ to rebuild."
    exit 0
fi

# -- musl-cross-make versions (pinned for reproducibility) --------------------

MCM_REPO="https://github.com/richfelker/musl-cross-make.git"
MCM_COMMIT="e5147dde912478dd32ad42a25003e82d4f5733aa"  # 2025-07-11
MCM_DIR="${PROJECT_DIR}/deps/musl-cross-make"

MCM_GCC_VER="14.2.0"
MCM_MUSL_VER="1.2.5"
MCM_BINUTILS_VER="2.44"
MCM_LINUX_VER="6.15.7"

# -- Build toolchain -----------------------------------------------------------

echo "  Target: ${MUSL_TRIPLE}"
echo "  GCC ${MCM_GCC_VER}, musl ${MCM_MUSL_VER}, binutils ${MCM_BINUTILS_VER}, linux ${MCM_LINUX_VER}"
echo "  This takes ~15-30 minutes on first build..."

# Clone musl-cross-make (shared across architectures)
if [ ! -d "${MCM_DIR}/.git" ]; then
    echo "  Cloning musl-cross-make..."
    git clone "${MCM_REPO}" "${MCM_DIR}"
fi
(cd "${MCM_DIR}" && git checkout -q "${MCM_COMMIT}")

# Generate config.mak
config_mak="${MCM_DIR}/config.mak"
cat > "$config_mak" <<CONFIGEOF
TARGET = ${MUSL_TRIPLE}
GCC_VER = ${MCM_GCC_VER}
MUSL_VER = ${MCM_MUSL_VER}
BINUTILS_VER = ${MCM_BINUTILS_VER}
LINUX_VER = ${MCM_LINUX_VER}
OUTPUT = ${TOOLCHAIN_DIR}
CONFIGEOF

# armv7l: target ARMv7-A hardware (RPi 2+), not the default ARMv5TE
if [ "${ARCH}" = "armv7l" ]; then
    cat >> "$config_mak" <<'ARMEOF'
COMMON_CONFIG += --with-arch=armv7-a --with-fpu=vfpv3-d16 --with-float=hard
ARMEOF
fi

echo "  Building toolchain (output: ${TOOLCHAIN_DIR})..."
make -C "${MCM_DIR}" -j"${NPROC}"
make -C "${MCM_DIR}" install

# Clean build artifacts to save disk space (keeps installed toolchain)
make -C "${MCM_DIR}" clean

# Verify the compiler works
if [ ! -x "${CC_PATH}" ]; then
    echo "Error: compiler not found after build: ${CC_PATH}" >&2
    exit 1
fi
echo "int main(){return 0;}" | "${CC_PATH}" -x c - -o /dev/null -static
echo "  Toolchain ready: ${CC_PATH}"

echo ""
echo "=== musl toolchain built for ${ARCH} ==="
echo "Compiler: ${CC_PATH}"
echo ""
echo "Next: cmake --preset musl  (or musl-aarch64 / musl-armv7)"
