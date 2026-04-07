# Implementation Gaps — 2026-04-07

This document identifies gaps between the toxcore-testnet project's stated goals and its current implementation, with actionable guidance for closing each gap.

---

## Go Test Node Cannot Build Against opd-ai/toxcore

- **Stated Goal**: Support go-toxcore (opd-ai/toxcore) as one of three Tox implementations in the cross-implementation testnet.

- **Current State**: The go-testnode (`cmd/go-testnode/main.go`) cannot compile. It references undefined methods on `*toxcore.Tox`:
  - `n.tox.IsConnected` (line 138)
  - `n.tox.FriendAdd` (lines 156, 199)
  - `n.tox.FriendIsConnected` (lines 163, 203, 209)
  - `n.tox.SetFriendMessageCallback` (line 230)
  - `n.tox.FriendSendMessage` returns 2 values, not 1 (line 212)
  - `MessageTypeNormal` cannot be used as string argument (line 212)

- **Impact**: 2 of 3 implementation pairs (go↔c, go↔rust) are completely untestable. The Go implementation is the primary use case for this testnet, yet it cannot participate in any compatibility tests.

- **Closing the Gap**:
  1. Clone opd-ai/toxcore locally and audit its exported API: `go doc github.com/opd-ai/toxcore`
  2. The actual opd-ai/toxcore API likely uses different method names. Research indicates:
     - Connection status may be via `Tox.SelfGetConnectionStatus()` or similar
     - Friend operations may be `AddFriend`, `AddFriendNorequest`, `FriendGetConnectionStatus`
     - Callbacks may use `OnFriendRequest`, `OnFriendMessage` patterns
  3. Update `cmd/go-testnode/main.go` to match the real API signatures
  4. Ensure `FriendSendMessage` return value is properly captured (returns `(uint32, error)`)
  5. Use the correct type for message type parameter
  6. Validate: `cd cmd/go-testnode && go mod edit -replace github.com/opd-ai/toxcore=<local-path> && go build -o /dev/null .`

---

## Rust Test Node Cannot Build Against tox-rs/tox

- **Stated Goal**: Support tox-rs/tox as one of three Tox implementations in the cross-implementation testnet.

- **Current State**: The rust-testnode (`testnode/tox_rs/src/main.rs`) fails to compile with 10 errors. The code imports `tox::toxcore::*` modules that no longer exist:
  - `tox::toxcore::crypto_core::SecretKey` — not found
  - `tox::toxcore::dht::packet::DhtPacket` — not found
  - `tox::toxcore::dht::server::Server` — not found
  - `tox::toxcore::friend_connection::FriendConnections` — not found
  - `tox::toxcore::net_crypto::NetCrypto` — not found
  - `tox::toxcore::onion::client::OnionClient` — not found
  - `tox::toxcore::tcp::client::Connections` — not found
  - `tox::toxcore::tox::Tox` — not found

- **Impact**: 2 of 3 implementation pairs (rust↔c, rust↔go) are completely untestable. The Rust implementation cannot participate in any compatibility tests.

- **Closing the Gap**:
  1. Research current tox-rs/tox API at https://docs.rs/tox and https://github.com/tox-rs/tox
  2. The library has been restructured into separate crates:
     - `tox_core` — main protocol logic
     - `tox_crypto` — cryptographic primitives
     - `tox_packet` — packet encoding/decoding
  3. Determine if tox-rs provides a high-level `Tox` struct or only low-level DHT/crypto primitives
  4. If high-level API exists: rewrite `Node` struct to use correct imports and method signatures
  5. If only low-level primitives exist: either build a wrapper that implements the required operations, or mark the Rust node as "unsupported" in CI (make it an optional build)
  6. Update `testnode/tox_rs/Cargo.toml` dependency specification if needed
  7. Validate: `cd testnode/tox_rs && cargo build --release`

---

## C Test Node Cannot Build Against TokTok/c-toxcore

- **Stated Goal**: Support TokTok/c-toxcore as one of three Tox implementations in the cross-implementation testnet.

- **Current State**: The c-toxcore CMake build fails during the configuration step:
  ```
  CMake Error at CMakeLists.txt:520 (add_library):
    Cannot find source file: third_party/cmp/cmp.c
  ```
  The c-toxcore repository requires git submodules to be initialized, but `scripts/build-ctoxcore.sh` uses `--depth=1` shallow clone without initializing submodules.

- **Impact**: All 3 implementation pairs are affected because c-toxcore is the reference implementation. The testnet cannot produce any compatibility results.

- **Closing the Gap**:
  1. Update `scripts/build-ctoxcore.sh` to initialize submodules after cloning:
     ```bash
     git clone --depth=1 --recurse-submodules \
         https://github.com/TokTok/c-toxcore.git \
         "${VENDOR_DIR}"
     ```
     Or add submodule init after clone:
     ```bash
     git -C "${VENDOR_DIR}" submodule update --init --recursive
     ```
  2. If submodules cause issues with shallow clone, consider removing `--depth=1` or using `--shallow-submodules`
  3. Validate: `bash scripts/build-ctoxcore.sh` completes without CMake errors

---

## File Transfer Test Not Implemented

- **Stated Goal**: Test file transfer functionality — transfer small binary blobs between implementations.

- **Current State**: All three test nodes explicitly return `not_implemented` for the `file_transfer` feature:
  - `cmd/go-testnode/main.go:251-252` — "file transfer API not yet confirmed in opd-ai/toxcore"
  - `testnode/ctoxcore/main.c:343-349` — "file transfer test not yet implemented in c-testnode"
  - `testnode/tox_rs/src/main.rs:328` — calls `not_impl()`

- **Impact**: File transfer compatibility cannot be verified. The test matrix shows 3 `not_implemented` results for file_transfer, providing no actionable compatibility data.

- **Closing the Gap**:
  1. Audit c-toxcore file transfer API: `tox_file_send`, `tox_file_send_chunk`, `tox_file_control`, and associated callbacks
  2. Implement `test_file_transfer` in c-testnode with a small binary blob roundtrip test
  3. Confirm file transfer API exists in opd-ai/toxcore; if present, implement in go-testnode
  4. Confirm file transfer API exists in tox-rs/tox; if present, implement in rust-testnode
  5. Validate: Run full test matrix; `file_transfer` shows "compatible" for at least c↔c pairs

---

## Conference/Group Chat Tests Not Implemented

- **Stated Goal**: Test conference invite and conference message functionality.

- **Current State**: All three test nodes explicitly return `not_implemented` for both conference features:
  - `conference_invite` — stubbed in all three nodes
  - `conference_message` — stubbed in all three nodes

- **Impact**: Group chat compatibility cannot be verified. 6 test matrix entries (2 features × 3 pairs) provide no actionable data.

- **Closing the Gap**:
  1. Audit c-toxcore conference API: `tox_conference_new`, `tox_conference_invite`, `tox_conference_join`, `tox_conference_send_message`, and callbacks
  2. Implement `test_conference_invite` and `test_conference_message` in c-testnode
  3. Audit opd-ai/toxcore for conference API support; implement if available
  4. Audit tox-rs/tox for conference API support; implement if available
  5. For implementations lacking conference support, ensure they correctly return `not_implemented`
  6. Validate: Conference tests show "compatible" or accurate "not_implemented" per implementation

---

## README Lacks Documentation

- **Stated Goal**: Provide a testnet that developers can use to verify cross-implementation interoperability.

- **Current State**: `README.md` contains only a single line: "a compatibility testnet for c-toxcore and go-toxcore". No build instructions, prerequisites, usage examples, or architecture documentation exists.

- **Impact**: New contributors cannot build or run the testnet locally. The project is effectively unusable without reading source code or CI workflow files.

- **Closing the Gap**:
  1. Add "Prerequisites" section listing: Go 1.22+, Rust stable, CMake, Ninja, libsodium-dev, pkg-config
  2. Add "Building" section with commands for each test node:
     ```bash
     # C node
     bash scripts/build-ctoxcore.sh
     
     # Rust node
     bash scripts/build-tox-rs.sh
     
     # Go node
     cd cmd/go-testnode
     go mod edit -replace github.com/opd-ai/toxcore=../../vendor/opd-ai-toxcore
     go build -o ../../bin/go-testnode .
     ```
  3. Add "Running Tests" section:
     ```bash
     go test -v ./integration/... -args \
       -go-node="${PWD}/bin/go-testnode" \
       -c-node="${PWD}/bin/c-testnode" \
       -rust-node="${PWD}/bin/rust-testnode"
     ```
  4. Add "Generating Reports" section explaining `cmd/report` usage
  5. Add "Architecture" section describing JSON-line IPC protocol
  6. Validate: README contains ≥500 words and all major sections

---

## CI Pipeline Has Zero Passing Builds

- **Stated Goal**: Run nightly CI to detect upstream implementation drift and produce compatibility reports.

- **Current State**: GitHub Actions workflow "Toxcore Cross-Implementation Testnet" has failed on all recent runs (runs #3 and #4 on main branch both have conclusion "failure"). All three build jobs fail before reaching the integration-tests job.

- **Impact**: The nightly drift detection is non-functional. Upstream API changes are not being caught, and no compatibility reports are being generated.

- **Closing the Gap**:
  1. Fix all three CRITICAL build issues documented in AUDIT.md
  2. Push fixes to main branch to trigger a new CI run
  3. Monitor workflow for success
  4. If any build fails, examine logs and iterate
  5. Once all builds pass, verify the integration-tests job runs and produces compatibility-report.json/md artifacts
  6. Validate: Workflow conclusion is "success" and compatibility report artifact is downloadable

---

*Generated by gaps analysis on 2026-04-07.*
