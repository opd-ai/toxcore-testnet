# AUDIT — 2026-04-07

## Project Goals

The toxcore-testnet project is a **compatibility testnet for c-toxcore, go-toxcore (opd-ai/toxcore), and tox-rs**. According to the README and ROADMAP.md, the project claims to:

1. **Spawn each Tox implementation as a subprocess** and coordinate them over a JSON-line IPC protocol
2. **Classify compatibility results** for each feature/implementation pair as "compatible", "conflicting", or "not_implemented"
3. **Test DHT bootstrap** — basic DHT connectivity between implementations
4. **Test friend request** — send and accept friend requests across implementations
5. **Test friend message** — exchange text messages between friends
6. **Test file transfer** — transfer small binary blobs (explicitly stubbed as not_implemented)
7. **Test conference invite** — invite peers into group conferences (explicitly stubbed as not_implemented)
8. **Test conference message** — broadcast messages in group conferences (explicitly stubbed as not_implemented)
9. **Generate deterministic reports** — produce JSON/Markdown compatibility matrices without LLM involvement
10. **Run nightly CI** — detect upstream implementation drift via scheduled GitHub Actions

**Target audience**: Developers maintaining or testing Tox protocol implementations who need to verify cross-implementation interoperability.

## Goal-Achievement Summary

| Goal | Status | Evidence |
|------|--------|----------|
| JSON-line IPC protocol for test nodes | ✅ Achieved | `integration/node.go:14-57`, `integration/doc.go:1-14` — complete protocol types defined |
| Classification of compatibility results | ✅ Achieved | `integration/report.go:30-69` — mechanical tri-state classification from exit codes |
| Deterministic report generation | ✅ Achieved | `cmd/report/main.go:101-113` — uses SOURCE_DATE_EPOCH, no LLM involvement |
| Nightly CI for upstream drift | ✅ Achieved | `.github/workflows/testnet.yml:9-11` — cron schedule at 02:00 UTC |
| Support c-toxcore (TokTok/c-toxcore) | ❌ Missing | CI fails: `CMakeLists.txt:520` — missing `third_party/cmp/cmp.c` source file |
| Support go-toxcore (opd-ai/toxcore) | ❌ Missing | CI fails: `cmd/go-testnode/main.go:138,156,163,199,203,209,212,230` — API mismatches |
| Support tox-rs/tox | ❌ Missing | CI fails: `testnode/tox_rs/src/main.rs:96-103` — `tox::toxcore::*` modules not found |
| Test DHT bootstrap | ⚠️ Partial | Test logic implemented in all 3 nodes; cannot verify until builds succeed |
| Test friend request | ⚠️ Partial | Test logic implemented in all 3 nodes; cannot verify until builds succeed |
| Test friend message | ⚠️ Partial | Test logic implemented in all 3 nodes; cannot verify until builds succeed |
| Test file transfer | ⚠️ Partial | Explicitly returns `not_implemented` in all nodes — design decision, not a bug |
| Test conference invite | ⚠️ Partial | Explicitly returns `not_implemented` in all nodes — design decision, not a bug |
| Test conference message | ⚠️ Partial | Explicitly returns `not_implemented` in all nodes — design decision, not a bug |

**Overall: 4/13 goals fully achieved, 6/13 partially achieved (blocked by build failures), 3/13 missing (build failures)**

## Findings

### CRITICAL

- [x] **Go test node API mismatch with opd-ai/toxcore** — `cmd/go-testnode/main.go:138,156,163,199,203,209,212,230` — The go-testnode assumes an opd-ai/toxcore API that doesn't exist. Methods like `IsConnected`, `FriendAdd`, `FriendIsConnected`, `SetFriendMessageCallback` are undefined. Additionally, `FriendSendMessage` signature mismatch: returns 2 values but code expects 1, and `MessageTypeNormal` cannot be used as string. **Remediation:** Audit the actual opd-ai/toxcore public API via `go doc github.com/opd-ai/toxcore` and update all method calls to match the real interface. Verify fix: `cd cmd/go-testnode && go mod edit -replace github.com/opd-ai/toxcore=<path> && go build .`

- [x] **Rust test node API mismatch with tox-rs/tox** — `testnode/tox_rs/src/main.rs:96-103` — The rust-testnode imports `tox::toxcore::*` modules that don't exist in current tox-rs/tox. The library has restructured; `toxcore` submodule no longer exists. Imports like `tox::toxcore::crypto_core::SecretKey`, `tox::toxcore::dht::*`, `tox::toxcore::tox::Tox` all fail resolution. **Remediation:** Research current tox-rs/tox API structure (now uses `tox_core`, `tox_crypto`, `tox_packet` crates), rewrite imports and Node implementation to match actual API, or mark Rust node as unsupported. Verify fix: `cd testnode/tox_rs && cargo build --release`

- [ ] **C test node build failure — missing c-toxcore source** — `scripts/build-ctoxcore.sh` + CI logs — c-toxcore CMake fails with "Cannot find source file: third_party/cmp/cmp.c". The TokTok/c-toxcore repository has restructured its submodules or build system. **Remediation:** Update `scripts/build-ctoxcore.sh` to initialize git submodules after clone: `git -C "${VENDOR_DIR}" submodule update --init --recursive`. Verify fix: `bash scripts/build-ctoxcore.sh`

### HIGH

- [ ] **No integration tests can run** — `integration/integration_test.go` — All 18 test cases (3 pairs × 6 features) are unreachable because none of the three test node binaries can be built. The test harness logic is complete, but the entire compatibility matrix produces no results. **Remediation:** Fix the three CRITICAL build issues above. Verify fix: `go test -v ./integration/... -args -go-node=bin/go-testnode -c-node=bin/c-testnode -rust-node=bin/rust-testnode`

- [ ] **CI pipeline fails on every run** — `.github/workflows/testnet.yml` — Workflow runs #3 and #4 on main branch both completed with conclusion "failure". All three build jobs fail, preventing the integration-tests job from producing meaningful results. **Remediation:** After fixing the three CRITICAL issues, push to trigger a new CI run. Verify fix: Check workflow conclusion is "success" in GitHub Actions.

### MEDIUM

- [ ] **High cyclomatic complexity in StartNode** — `integration/node.go:74` — Complexity 14, overall score 19.2. The function handles process spawning, pipe setup, goroutine management, and ready-message parsing in a single 56-line function with deeply nested select/case statements. **Remediation:** Extract `readReadyMessage()` helper function to handle the ready-message parsing loop (lines 113-128). Verify fix: `go-stats-generator analyze . --skip-tests | grep StartNode` shows complexity ≤10.

- [ ] **High cyclomatic complexity in main (go-testnode)** — `cmd/go-testnode/main.go:297` — Complexity 12, 101 lines, overall score 17.1. The function combines Tox initialization, callback registration, ready-message emission, signal handling, and the entire command dispatch loop. **Remediation:** Extract `dispatchCommand()` helper for the switch statement on `env.Cmd` (lines 363-394). Verify fix: `go-stats-generator analyze . --skip-tests | grep "main.*cmd/go-testnode"` shows complexity ≤10.

- [ ] **High cyclomatic complexity in testFriendMessage** — `cmd/go-testnode/main.go:190` — Complexity 11, overall score 16.3. The function handles both initiator and responder roles with multiple nested polling loops. **Remediation:** Split into `testFriendMessageInitiator()` and `testFriendMessageResponder()` helper methods. Verify fix: `go-stats-generator analyze . --skip-tests | grep testFriendMessage` shows complexity ≤8.

### LOW

- [ ] **Single-letter variable names in hot paths** — `cmd/go-testnode/main.go:365,380`, `cmd/report/main.go:218`, `integration/node.go:177,183` — Variables named `c`, `r`, `e` violate Go naming conventions. These should use descriptive names in non-trivial scopes. **Remediation:** Rename `c` to `cmd` or `bootstrapCmd`/`runTestCmd`, `r` to `result` or `testResult`, `e` to `err` or `parseErr`. Verify fix: `go-stats-generator analyze . --skip-tests | grep "single_le"` returns no results.

- [ ] **README lacks usage instructions** — `README.md:1-2` — The README contains only a one-line description: "a compatibility testnet for c-toxcore and go-toxcore". No build instructions, prerequisites, or usage examples. **Remediation:** Expand README with: prerequisites (Go 1.22+, Rust stable, CMake, libsodium-dev), build instructions for each node, test execution commands, and report interpretation guide. Verify fix: README contains "## Prerequisites", "## Building", "## Running Tests" sections.

- [ ] **ROADMAP.md duplicates audit content** — `ROADMAP.md:1-168` — The existing ROADMAP.md file contains a detailed goal-achievement assessment that overlaps significantly with this audit. This creates maintenance burden and potential for drift between documents. **Remediation:** Keep ROADMAP.md focused on future work items only; remove the "Goal-Achievement Assessment" section since it's now superseded by AUDIT.md. Verify fix: ROADMAP.md starts with "## Roadmap" not "## Goal-Achievement Assessment".

## Metrics Snapshot

| Metric | Value |
|--------|-------|
| Total Lines of Code | 543 |
| Total Functions | 19 |
| Total Methods | 11 |
| Total Packages | 2 |
| Total Files | 5 |
| Average Function Length | 20.5 lines |
| Average Complexity | 6.0 |
| Functions with Complexity >10 | 3 |
| Longest Function | `main` (101 lines) |
| Documentation Coverage | 100% |
| Naming Convention Score | 1.00 |
| `go vet` Warnings | 0 |
| `go build` Status | ✅ Passes (harness code only) |
| CI Status | ❌ Failing (all 3 test node builds fail) |

### Complexity Hotspots

| Function | File | Lines | Cyclomatic | Overall Score |
|----------|------|-------|------------|---------------|
| StartNode | integration/node.go | 56 | 14 | 19.2 |
| main | cmd/go-testnode/main.go | 101 | 12 | 17.1 |
| testFriendMessage | cmd/go-testnode/main.go | 52 | 11 | 16.3 |
| RunTest | integration/node.go | 50 | 10 | 15.0 |
| loadResults | cmd/report/main.go | 48 | 10 | 14.5 |

---

*Generated by functional audit on 2026-04-07. Metrics from go-stats-generator v1.0.0.*
