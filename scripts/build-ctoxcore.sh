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

# ── Clone c-toxcore (skip if already present) ──────────────────────────────
if [[ ! -d "${VENDOR_DIR}/.git" ]]; then
    echo "[build-ctoxcore] Cloning TokTok/c-toxcore..."
    git clone --depth=1 \
        https://github.com/TokTok/c-toxcore.git \
        "${VENDOR_DIR}"
else
    echo "[build-ctoxcore] c-toxcore already cloned; skipping."
fi

# ── Build c-toxcore ────────────────────────────────────────────────────────
BUILD_DIR="${VENDOR_DIR}/_build"
if [[ ! -f "${BUILD_DIR}/libtoxcore.a" && ! -f "${BUILD_DIR}/libtoxcore.so" ]]; then
    echo "[build-ctoxcore] Configuring c-toxcore with CMake..."
    cmake -B "${BUILD_DIR}" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${INSTALL_DIR}" \
        -DBUILD_TOXAV=OFF \
        -DENABLE_STATIC=ON \
        -DENABLE_SHARED=ON \
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
cmake -B "${TESTNODE_BUILD}" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="${INSTALL_DIR}" \
    -DCMAKE_INSTALL_PREFIX="${REPO_ROOT}" \
    "${TESTNODE_DIR}"

cmake --build "${TESTNODE_BUILD}" --parallel "$(nproc)"
cmake --install "${TESTNODE_BUILD}"

echo "[build-ctoxcore] c-testnode installed to ${BIN_DIR}/c-testnode"
