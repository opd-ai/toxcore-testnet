# Implementation Gaps — 2026-04-07

This document identifies gaps between the toxcore-testnet project's stated goals and its current implementation, with actionable guidance for closing each gap.

---

## 1. go-toxcore ↔ c-toxcore DHT Connectivity Fails

- **Stated Goal**: Test DHT bootstrap, friend request, and friend message features across all three implementation pairs, with the go↔c pair being the primary use case.

- **Current State**: All recent CI runs (22+ consecutive failures) show the go-toxcore↔c-toxcore pair timing out on friend_request and friend_message tests. The harness sends bootstrap commands with the port from go-testnode's ready message (hardcoded to 33445 at `cmd/go-testnode/main.go:47`), but if opd-ai/toxcore binds to a different port, c-testnode's bootstrap request goes to the wrong address.

- **Impact**: The testnet's core purpose—verifying cross-implementation compatibility—is not achieved for the most important pair. CI status cannot be trusted as a health indicator.

- **Closing the Gap**:
  1. Investigate whether opd-ai/toxcore provides `SelfGetUDPPort()` or equivalent API
  2. If available, replace `n.udpPort = defaultUDPPort` with `n.udpPort = t.SelfGetUDPPort()` in `cmd/go-testnode/main.go:374`
  3. If not available, use ToxOptions to force binding to the configured port and fail fast if binding fails
  4. Add stderr logging in go-testnode showing actual bound port for debugging
  5. Verify fix by running `netstat -ulnp | grep go-testnode` immediately after startup

---

## 2. Rust Test Node Is Entirely Stubbed

- **Stated Goal**: Support tox-rs/tox as one of three Tox implementations in the cross-implementation testnet (README.md lines 3-4, 45-57).

- **Current State**: The rust-testnode (`testnode/tox_rs/src/main.rs`) is explicitly non-functional. Lines 7-11 document: "The tox-rs/tox library has restructured and no longer provides a high-level Tox struct API." The tox dependency is commented out in `Cargo.toml:12-16`. All tests emit `not_implemented` with exit code 2.

- **Impact**: 
  - 2 of 3 implementation pairs (rust↔c, rust↔go) produce no compatibility data
  - 12 of 18 test matrix entries are automatically `not_implemented`
  - README claims "tox-rs implementation" support which is misleading

- **Closing the Gap**:
  1. Monitor https://github.com/tox-rs/tox for high-level API development
  2. Alternative: wrap c-toxcore via FFI from Rust using `libc` crate as interim solution
  3. Update README.md lines 45-57 to add: **Note: The Rust test node is currently non-functional pending upstream API availability.**
  4. Consider removing rust-testnode from CI (`-rust-node` flag) to reduce report noise until functional
  5. Track upstream in ROADMAP.md Priority 3 (already documented)

---

## 3. File Transfer Feature Not Implemented

- **Stated Goal**: Test file transfer functionality ("transfer a small binary blob") per README.md line 109.

- **Current State**: All three test nodes explicitly return `not_implemented` for `file_transfer`:
  - `cmd/go-testnode/main.go:269-271`: "file transfer API not yet confirmed in opd-ai/toxcore"
  - `testnode/ctoxcore/main.c:342-350`: "file transfer test not yet implemented in c-testnode"
  - rust-testnode stubs all features

- **Impact**: 3 test matrix entries (go↔c, go↔rust, c↔rust) show `not_implemented`. File transfer compatibility cannot be verified despite being listed as a testable feature.

- **Closing the Gap**:
  1. Implement in c-testnode first using stable c-toxcore API:
     - `tox_file_send()` / `tox_file_send_chunk()` for sending
     - `tox_file_control()` / `tox_file_recv_chunk()` callbacks for receiving
  2. Test protocol:
     - Initiator sends 1KB test blob
     - Responder accepts, receives chunks, sends confirmation message
     - Initiator verifies confirmation
  3. Audit opd-ai/toxcore for file transfer API: `go doc github.com/opd-ai/toxcore | grep -i file`
  4. Implement in go-testnode once c-testnode works
  5. Update README to mark file_transfer as "🚧 Planned" until implemented

---

## 4. Conference/Group Chat Features Not Implemented

- **Stated Goal**: Test conference invite and conference message functionality per README.md lines 110-111.

- **Current State**: Both `conference_invite` and `conference_message` return `not_implemented` in all three nodes:
  - `cmd/go-testnode/main.go:277-286`
  - `testnode/ctoxcore/main.c:352-364`
  - rust-testnode stubs all features

- **Impact**: 6 test matrix entries (2 features × 3 pairs) provide no actionable data. Group chat compatibility cannot be verified.

- **Closing the Gap**:
  1. Implement in c-testnode using c-toxcore conference API:
     - `tox_conference_new()` — create conference
     - `tox_conference_invite()` — invite peer
     - `tox_conference_join()` — accept invitation
     - `tox_conference_send_message()` — send message
     - Associated callbacks for receiving invites/messages
  2. Test protocol for conference_invite:
     - Initiator creates conference, invites responder
     - Responder accepts via callback
     - Both verify conference membership
  3. Test protocol for conference_message:
     - Both join conference
     - Initiator sends message
     - Responder verifies receipt
  4. Audit opd-ai/toxcore for conference API before implementing go-testnode
  5. Update README to mark conference features as "🚧 Planned"

---

## 5. README Documents Unimplemented Features as Supported

- **Stated Goal**: README should accurately describe the project's capabilities.

- **Current State**: README.md lines 104-112 list all 6 test features in a table without indicating implementation status. The note on line 58 mentions Rust stubbing, but the feature table implies all features are functional.

- **Impact**: Users cloning the repo expect full functionality but find 50% of features stubbed.

- **Closing the Gap**:
  1. Add "Status" column to feature table at README.md:104-112:
     ```markdown
     | Feature | Description | Status |
     |---------|-------------|--------|
     | `dht_bootstrap` | DHT connectivity | ✅ Implemented |
     | `friend_request` | Send/accept friend requests | ✅ Implemented |
     | `friend_message` | Exchange text messages | ✅ Implemented |
     | `file_transfer` | Transfer binary blobs | 🚧 Planned |
     | `conference_invite` | Group invitations | 🚧 Planned |
     | `conference_message` | Group messaging | 🚧 Planned |
     ```
  2. Add prominent note about Rust node status near line 45
  3. Update after each feature is implemented

---

## 6. CI Workflow Go Version Mismatch

- **Stated Goal**: CI should build and test against the Go version specified in go.mod.

- **Current State**: `.github/workflows/testnet.yml:20` sets `GO_VERSION: '1.22'` but both `go.mod:3` and `cmd/go-testnode/go.mod:3` require `go 1.25.0`.

- **Impact**: CI may succeed with Go 1.22 features but fail when users build with Go 1.25, or vice versa. The mismatch can mask compatibility issues.

- **Closing the Gap**:
  1. Update `.github/workflows/testnet.yml:20` to `GO_VERSION: '1.25'`
  2. Verify CI passes with the updated version
  3. Consider using `go mod download` to validate module compatibility

---

## 7. C Test Node RPATH Uses Absolute Path

- **Stated Goal**: c-testnode binary should be portable across CI job runners.

- **Current State**: `testnode/ctoxcore/CMakeLists.txt:24-26` sets RPATH to `${TOXCORE_LIBRARY_DIRS}`, which resolves to an absolute path like `/home/user/.../vendor/c-toxcore-install/lib`. When the binary is uploaded as an artifact and downloaded in a different runner, this path doesn't exist.

- **Impact**: c-testnode crashes on startup in the integration-tests job because libtoxcore.so is not found. PR #3 notes this was previously fixed by changing RPATH to `$ORIGIN/lib` and bundling shared libraries.

- **Closing the Gap**:
  1. Verify `CMakeLists.txt:24-26` uses `$ORIGIN/lib` not absolute path
  2. Ensure `scripts/build-ctoxcore.sh` bundles `libtoxcore.so*` into `bin/lib/`
  3. Ensure CI workflow uploads `bin/lib/` alongside `bin/c-testnode`
  4. Test locally: `ldd bin/c-testnode` should show `libtoxcore.so => ./lib/libtoxcore.so`

---

## 8. Protocol Type Duplication Across Implementations

- **Stated Goal**: IPC protocol should be consistently implemented across all nodes (noted in ROADMAP.md Priority 4).

- **Current State**: IPC types are defined independently in:
  - `integration/node.go:15-58` (Go harness types)
  - `cmd/go-testnode/main.go:52-89` (Go node types)
  - `testnode/ctoxcore/main.c` (inline structs)
  - `testnode/tox_rs/src/main.rs:31-73` (Rust types)

- **Impact**: Protocol changes require updates in 4 places. Drift between definitions can cause silent failures.

- **Closing the Gap**:
  1. Extract IPC types into a shared `protocol/` package (Go)
  2. Generate C header from Go definitions using `cgo -exportheader` or manual generation
  3. Generate Rust types using JSON schema or manual port
  4. Add protocol version field to ready message for compatibility checking
  5. This is documented as ROADMAP.md Priority 4 (low priority)

---

*Generated by functional audit on 2026-04-07.*
