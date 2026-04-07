# Goal-Achievement Assessment

## Project Context
- **What it claims to do**: A compatibility testnet for c-toxcore and go-toxcore (and tox-rs) — spawns each implementation as a subprocess, coordinates them over a JSON-line IPC protocol, and classifies each feature/pair combination as "compatible", "conflicting", or "not_implemented"
- **Target audience**: Developers maintaining or testing Tox protocol implementations (c-toxcore, opd-ai/toxcore, tox-rs/tox) who need to verify cross-implementation interoperability
- **Architecture**:
  - `integration/` — Go test harness that starts node subprocesses, sends IPC commands, and classifies compatibility results
  - `cmd/go-testnode/` — Go test node wrapping opd-ai/toxcore
  - `cmd/report/` — Report generator producing JSON/Markdown compatibility matrices
  - `testnode/ctoxcore/` — C test node wrapping TokTok/c-toxcore
  - `testnode/tox_rs/` — Rust test node wrapping tox-rs/tox
  - `scripts/` — Build scripts for C and Rust dependencies
- **Existing CI/quality gates**: GitHub Actions workflow (`testnet.yml`) builds all 3 nodes, runs full compatibility matrix, generates reports, and enforces no "conflicting" results

## Goal-Achievement Summary

| Stated Goal | Status | Evidence | Gap Description |
|-------------|--------|----------|-----------------|
| Cross-implementation compatibility testnet | ⚠️ Partial | Framework complete, CI workflow defined | Builds currently fail due to API mismatches with upstream dependencies |
| JSON-line IPC protocol for test nodes | ✅ Achieved | `integration/node.go`, `integration/doc.go` define complete protocol | — |
| Support go-toxcore (opd-ai/toxcore) | ❌ Missing | `cmd/go-testnode/main.go` references undefined methods | opd-ai/toxcore API differs from expected interface |
| Support c-toxcore (TokTok/c-toxcore) | ✅ Achieved | `testnode/ctoxcore/main.c` compiles and runs (CI passes for this job) | — |
| Support tox-rs/tox | ❌ Missing | `testnode/tox_rs/src/main.rs` fails compilation | tox-rs API changed; module paths like `tox::toxcore::*` no longer exist |
| Test DHT bootstrap | ⚠️ Partial | Test logic implemented in all 3 nodes | Cannot verify until builds succeed |
| Test friend request | ⚠️ Partial | Test logic implemented in all 3 nodes | Cannot verify until builds succeed |
| Test friend message | ⚠️ Partial | Test logic implemented in all 3 nodes | Cannot verify until builds succeed |
| Test file transfer | ⚠️ Partial | Explicitly returns `not_implemented` in all nodes | Designed as placeholder; no implementation |
| Test conference invite | ⚠️ Partial | Explicitly returns `not_implemented` in all nodes | Designed as placeholder; no implementation |
| Test conference message | ⚠️ Partial | Explicitly returns `not_implemented` in all nodes | Designed as placeholder; no implementation |
| Deterministic report generation | ✅ Achieved | `cmd/report/main.go` uses SOURCE_DATE_EPOCH, no AI/LLM | — |
| Nightly CI for upstream drift | ✅ Achieved | `testnet.yml` cron schedule at 02:00 UTC | — |

**Overall: 4/12 goals fully achieved, 6/12 partially achieved, 2/12 missing**

---

## Roadmap

### Priority 1: Fix Go Test Node API Compatibility

**Impact**: Blocking — go-testnode cannot compile, making 2/3 implementation pairs untestable

The go-testnode (`cmd/go-testnode/main.go`) assumes an opd-ai/toxcore API that does not match the actual library:
- `n.tox.IsConnected` → undefined
- `n.tox.FriendAdd` → undefined
- `n.tox.FriendIsConnected` → undefined
- `n.tox.SetFriendMessageCallback` → undefined

**Root cause**: The testnode was written against an assumed API without verifying the actual opd-ai/toxcore interface.

- [ ] Clone opd-ai/toxcore and audit its public API (`go doc github.com/opd-ai/toxcore`)
- [ ] Update `cmd/go-testnode/main.go` to use correct method names and signatures
  - Lines 138, 156, 163, 199, 203, 209, 212, 230 contain API mismatches
- [ ] Fix `FriendSendMessage` signature mismatch (returns 2 values, not 1)
- [ ] Fix `MessageType` usage — cannot use constant as string
- [ ] Validate: `cd cmd/go-testnode && go build -o /dev/null .` succeeds

### Priority 2: Fix Rust Test Node API Compatibility

**Impact**: Blocking — rust-testnode cannot compile, making 2/3 implementation pairs untestable

The rust-testnode (`testnode/tox_rs/src/main.rs`) imports modules that don't exist in current tox-rs/tox:
- `tox::toxcore::crypto_core::SecretKey` → not found
- `tox::toxcore::dht::*` → not found
- `tox::toxcore::friend_connection::*` → not found
- `tox::toxcore::tox::Tox` → not found

**Root cause**: tox-rs/tox restructured its public API; the old `toxcore` module hierarchy no longer exists.

- [ ] Research current tox-rs/tox API: check `tox-rs/tox` GitHub master branch and docs
- [ ] Determine if tox-rs provides a high-level `Tox` struct or only low-level primitives
- [ ] Rewrite `testnode/tox_rs/src/main.rs` to match actual API, or
- [ ] If tox-rs lacks required features, mark the Rust node as unsupported and update CI to make it optional
- [ ] Validate: `cd testnode/tox_rs && cargo build --release` succeeds

### Priority 3: Implement File Transfer Test

**Impact**: Medium — expands feature coverage from 3/6 to 4/6 tested features

All three nodes explicitly return `not_implemented` for `file_transfer`:
- `cmd/go-testnode/main.go:251-252` — "file transfer API not yet confirmed in opd-ai/toxcore"
- `testnode/ctoxcore/main.c:343-349` — "file transfer test not yet implemented in c-testnode"
- `testnode/tox_rs/src/main.rs:328` — calls `not_impl()`

- [ ] Audit c-toxcore file transfer API: `tox_file_send`, `tox_file_send_chunk`, callbacks
- [ ] Implement `test_file_transfer` in c-testnode with small binary blob roundtrip
- [ ] Implement corresponding test in go-testnode (after API is confirmed in opd-ai/toxcore)
- [ ] Implement corresponding test in rust-testnode (after API compatibility is resolved)
- [ ] Validation: run full test matrix, verify `file_transfer` shows "compatible" for c↔c pairs

### Priority 4: Implement Conference/Group Chat Tests

**Impact**: Medium — expands feature coverage to 6/6 tested features

Conference invite and message tests are stubbed as `not_implemented` in all nodes:
- c-toxcore has conference API (`tox_conference_new`, `tox_conference_invite`, etc.)
- opd-ai/toxcore and tox-rs status unknown

- [ ] Audit opd-ai/toxcore for conference API support
- [ ] Audit tox-rs/tox for conference API support
- [ ] Implement `test_conference_invite` in c-testnode
- [ ] Implement `test_conference_message` in c-testnode
- [ ] Port implementations to other nodes as APIs become available
- [ ] Validation: conference tests show "compatible" or accurate "not_implemented" per implementation

### Priority 5: Improve Code Maintainability

**Impact**: Low — code health improvements; does not affect functionality

Metrics from go-stats-generator identify modest code health improvements:

| Metric | Current | Target | File |
|--------|---------|--------|------|
| Cyclomatic complexity | 14 | ≤10 | `integration/node.go:StartNode` |
| Cyclomatic complexity | 12 | ≤10 | `cmd/go-testnode/main.go:main` |
| Function length | 101 lines | ≤50 | `cmd/go-testnode/main.go:main` |
| Documentation | 100% | maintain | all files |

- [ ] Extract `readReadyMessage()` helper from `StartNode()` to reduce complexity
- [ ] Extract `dispatchCommand()` helper from `main()` in go-testnode
- [ ] Add package-level documentation to `cmd/go-testnode/` explaining build workflow
- [ ] Consider extracting IPC types into a shared `protocol/` package to avoid duplication across nodes

### Priority 6: Add Local Development Instructions

**Impact**: Low — improves contributor onboarding

The README contains only a one-line description. Contributors need guidance to:
- Build all three test nodes locally
- Run the full compatibility matrix
- Interpret report output

- [ ] Expand README.md with:
  - Prerequisites (Go 1.22+, Rust stable, CMake, libsodium-dev)
  - Build instructions for each node
  - How to run tests locally: `go test -v ./integration/... -args -go-node=... -c-node=...`
  - How to generate reports manually
- [ ] Add example output from a successful compatibility run

---

## Appendix: Metrics Summary

**Codebase size**: 543 lines of Go code across 5 files (plus C and Rust nodes)

**Complexity hotspots** (cyclomatic complexity >10):
- `StartNode` (integration/node.go): 14
- `main` (cmd/go-testnode/main.go): 12
- `testFriendMessage` (cmd/go-testnode/main.go): 11
- `RunTest` (integration/node.go): 10
- `loadResults` (cmd/report/main.go): 10

**Documentation coverage**: 100% (all exported symbols documented)

**Static analysis**: `go vet ./...` passes with no warnings

**CI status**: Failing on `main` branch — Go and Rust builds fail due to upstream API mismatches

---

## Notes

- The C test node (c-testnode) compiles successfully; c-toxcore integration is the most complete
- The JSON-line IPC protocol is well-designed and consistently implemented across nodes
- The report generator is deterministic and correctly avoids any AI/LLM usage
- Feature tests for file_transfer and conferences are intentionally stubbed as placeholders
- No open issues exist in the repository; this roadmap serves as the initial backlog
