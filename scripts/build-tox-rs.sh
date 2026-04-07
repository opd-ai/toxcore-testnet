#!/usr/bin/env bash
# scripts/build-tox-rs.sh
#
# Builds the Rust test node (testnode/tox_rs) which depends on tox-rs/tox.
#
# Artefacts:
#   bin/rust-testnode   — ready-to-run test node binary
#
# Prerequisites:
#   Rust toolchain (stable) — installed by the CI via dtolnay/rust-toolchain

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TESTNODE_DIR="${REPO_ROOT}/testnode/tox_rs"
BIN_DIR="${REPO_ROOT}/bin"

mkdir -p "${BIN_DIR}"

echo "[build-tox-rs] Building Rust test node..."
cd "${TESTNODE_DIR}"
cargo build --release

cp "target/release/rust-testnode" "${BIN_DIR}/rust-testnode"
echo "[build-tox-rs] rust-testnode installed to ${BIN_DIR}/rust-testnode"
