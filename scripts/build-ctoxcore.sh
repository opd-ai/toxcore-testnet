#!/usr/bin/env bash
# scripts/build-ctoxcore.sh
#
# Clones TokTok/c-toxcore, builds it with CMake, then builds the C test node
# (testnode/ctoxcore) against the installed library.
#
# Artefacts:
#   vendor/c-toxcore/     — c-toxcore source + build directory (cached)
#   bin/c-testnode        — ready-to-run test node binary
#
# Prerequisites (Ubuntu / Debian):
#   sudo apt-get install -y cmake ninja-build libsodium-dev pkg-config

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENDOR_DIR="${REPO_ROOT}/vendor/c-toxcore"
INSTALL_DIR="${REPO_ROOT}/vendor/c-toxcore-install"
BIN_DIR="${REPO_ROOT}/bin"
TESTNODE_DIR="${REPO_ROOT}/testnode/ctoxcore"

mkdir -p "${BIN_DIR}" "${INSTALL_DIR}"

# ── Clone or update c-toxcore ─────────────────────────────────────────────
if [[ ! -d "${VENDOR_DIR}/.git" ]]; then
    echo "[build-ctoxcore] Cloning TokTok/c-toxcore..."
    git clone --depth=1 \
        https://github.com/TokTok/c-toxcore.git \
        "${VENDOR_DIR}"
    # Initialize submodules (required for third_party/cmp)
    echo "[build-ctoxcore] Initializing submodules..."
    git -C "${VENDOR_DIR}" submodule update --init --recursive --depth=1
else
    echo "[build-ctoxcore] Updating existing c-toxcore checkout..."
    PREVIOUS_HEAD="$(git -C "${VENDOR_DIR}" rev-parse HEAD 2>/dev/null || true)"
    git -C "${VENDOR_DIR}" fetch --depth=1 origin
    git -C "${VENDOR_DIR}" reset --hard FETCH_HEAD
    # Update submodules in case they changed
    git -C "${VENDOR_DIR}" submodule update --init --recursive --depth=1
    CURRENT_HEAD="$(git -C "${VENDOR_DIR}" rev-parse HEAD)"

    if [[ "${PREVIOUS_HEAD}" != "${CURRENT_HEAD}" ]]; then
        echo "[build-ctoxcore] c-toxcore updated (${PREVIOUS_HEAD:-none} -> ${CURRENT_HEAD}); removing cached build output."
        rm -rf "${VENDOR_DIR}/_build"
    else
        echo "[build-ctoxcore] c-toxcore already up to date."
    fi
fi

# ── Build c-toxcore ────────────────────────────────────────────────────────
BUILD_DIR="${VENDOR_DIR}/_build"
if [[ ! -f "${BUILD_DIR}/libtoxcore.a" ]]; then
    echo "[build-ctoxcore] Configuring c-toxcore with CMake..."
    cmake -B "${BUILD_DIR}" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
        -DBUILD_TOXAV=OFF \
        -DENABLE_STATIC=ON \
        -DENABLE_SHARED=OFF \
        "${VENDOR_DIR}"

    echo "[build-ctoxcore] Building c-toxcore..."
    cmake --build "${BUILD_DIR}" --parallel "$(nproc)"

    echo "[build-ctoxcore] Installing c-toxcore to ${INSTALL_DIR}..."
    cmake --install "${BUILD_DIR}"
else
    echo "[build-ctoxcore] c-toxcore already built; skipping."
    # Re-install in case the install dir was not cached.
    cmake --install "${BUILD_DIR}"
fi

# ── Build the C test node ──────────────────────────────────────────────────
TESTNODE_BUILD="${REPO_ROOT}/vendor/c-testnode-build"
echo "[build-ctoxcore] Building C test node..."

# Set PKG_CONFIG_PATH so pkg-config finds the locally installed libtoxcore
export PKG_CONFIG_PATH="${INSTALL_DIR}/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

cmake -B "${TESTNODE_BUILD}" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="${INSTALL_DIR}" \
    -DCMAKE_INSTALL_PREFIX="${REPO_ROOT}" \
    "${TESTNODE_DIR}"

cmake --build "${TESTNODE_BUILD}" --parallel "$(nproc)"
cmake --install "${TESTNODE_BUILD}"

# ── Verify static binary ──────────────────────────────────────────────────
# With ENABLE_SHARED=OFF the c-testnode links libtoxcore statically, so no
# shared-library bundling is needed. Verify the binary does not depend on
# libtoxcore.so at runtime.
if ldd "${BIN_DIR}/c-testnode" 2>/dev/null | grep -q libtoxcore; then
    echo "[build-ctoxcore] WARNING: c-testnode still dynamically links libtoxcore"
else
    echo "[build-ctoxcore] OK: c-testnode does not dynamically link libtoxcore"
fi

echo "[build-ctoxcore] c-testnode installed to ${BIN_DIR}/c-testnode"
