# AUDIT — 2026-04-07

## Project Goals

Based on README.md and ROADMAP.md, toxcore-testnet is a **compatibility testnet for c-toxcore, go-toxcore (opd-ai/toxcore), and tox-rs implementations**. The project:

1. **Spawns each Tox implementation as a subprocess** coordinated via a JSON-line IPC protocol
2. **Classifies feature compatibility** as "compatible", "conflicting", or "not_implemented"
3. **Tests 6 protocol features** across 3 implementation pairs (18 test matrix entries):
   - dht_bootstrap, friend_request, friend_message, file_transfer, conference_invite, conference_message
4. **Runs nightly CI** to detect upstream implementation drift
5. **Generates deterministic reports** (JSON + Markdown) without any LLM/AI involvement
6. **Supports all three implementations**: go-toxcore (Go), c-toxcore (C), tox-rs (Rust)

**Target Audience**: Developers maintaining Tox protocol implementations who need cross-compatibility verification.

---

## Goal-Achievement Summary

| Goal | Status | Evidence |
|------|--------|----------|
| JSON-line IPC protocol implementation | ✅ Achieved | integration/node.go:15-58 defines protocol types; all 3 test nodes implement them |
| Launch test nodes as subprocesses | ✅ Achieved | integration/node.go:95-140 `StartNode()` manages subprocess lifecycle |
| Classify compatibility (3 states) | ✅ Achieved | integration/report.go:22-69 `classifyResults()` maps exit codes to status |
| Test dht_bootstrap feature | ✅ Achieved | cmd/go-testnode/main.go:139-159, testnode/ctoxcore/main.c:214-229 |
| Test friend_request feature | ✅ Achieved | cmd/go-testnode/main.go:163-197, testnode/ctoxcore/main.c:231-278 |
| Test friend_message feature | ✅ Achieved | cmd/go-testnode/main.go:199-261, testnode/ctoxcore/main.c:280-340 |
| Test file_transfer feature | ⚠️ Partial | Stubbed as `not_implemented` in all 3 nodes |
| Test conference_invite feature | ⚠️ Partial | Stubbed as `not_implemented` in all 3 nodes |
| Test conference_message feature | ⚠️ Partial | Stubbed as `not_implemented` in all 3 nodes |
| Support tox-rs (Rust) node | ❌ Missing | testnode/tox_rs/src/main.rs:1-11 explicitly stubs all functionality |
| Nightly CI detects drift | ⚠️ Partial | CI runs but all recent runs fail due to timeouts |
| Deterministic report generation | ✅ Achieved | cmd/report/main.go:11-15 confirms no LLM usage; reproducible timestamps |
| Report uploaded to GitHub Pages | ✅ Achieved | .github/workflows/testnet.yml handles artifact upload |

---

## Findings

### CRITICAL

- [x] **CI pipeline consistently fails due to go↔c test timeouts** — `.github/workflows/testnet.yml` — All 22+ recent CI runs conclude with "failure" status. The go-toxcore↔c-toxcore pair times out on friend_request and friend_message tests (90s timeout exceeded). Root cause is likely incorrect port reporting in go-testnode. **Remediation:** Fix the hardcoded UDP port issue (see HIGH finding below), then verify CI passes with `go test -v ./integration/... -args -go-node=bin/go-testnode -c-node=bin/c-testnode`.

### HIGH

- [x] **Hardcoded UDP port in go-testnode** — `cmd/go-testnode/main.go:47,374,401-403` — The constant `defaultUDPPort = 33445` is used in the ready message regardless of actual bound port. If port 33445 is unavailable, opd-ai/toxcore may bind elsewhere but report 33445, causing bootstrap failures. c-testnode correctly uses `tox_self_get_udp_port()` at line 456. **Remediation:** Check if opd-ai/toxcore exposes `SelfGetUDPPort()` or similar; if so, replace `n.udpPort` with the actual value. If not, add port-range validation to detect binding failures. Verify with `netstat -ulnp | grep testnode` after startup.

- [ ] **tox-rs test node is non-functional (stubs all tests)** — `testnode/tox_rs/src/main.rs:7-11,92,126-136` — The Rust node explicitly reports all features as `not_implemented` with the reason "tox-rs/tox library restructured; high-level Tox API not available". This makes 12/18 test matrix entries (6 features × 2 rust pairs) provide no useful data. **Remediation:** Monitor tox-rs/tox for HLAPI availability. In the interim, either (a) remove rust-testnode from CI to reduce noise, or (b) document prominently in README that Rust support is disabled. Restore when upstream provides a usable API.

- [ ] **3/6 features return `not_implemented` in all nodes** — `cmd/go-testnode/main.go:269-286`, `testnode/ctoxcore/main.c:342-364` — file_transfer, conference_invite, and conference_message are stubbed in both functional nodes (Go and C). This means 9/18 test matrix entries provide no compatibility data. **Remediation:** Implement file_transfer in c-testnode first (c-toxcore has stable API: `tox_file_send`, `tox_file_send_chunk`, callbacks). Then port to go-testnode if opd-ai/toxcore supports file transfer.

### MEDIUM

- [x] **README lists unimplemented features without status indication** — `README.md:104-112` — The feature table lists all 6 features without indicating that 3 are not yet implemented. Users cloning the repo may expect full functionality. **Remediation:** Add a "Status" column to the feature table distinguishing "✅ Implemented" from "🚧 Planned".

- [ ] **`StartNode` function has elevated complexity** — `integration/node.go:95-140` — Cyclomatic complexity 9, overall score 12.7 (highest in codebase). The function combines process spawning, pipe setup, goroutine creation, and ready message handling. **Remediation:** Extract `readReadyMessage` call and goroutine setup into a helper function `initializeNodeIO()`. Verify with `go-stats-generator analyze . --format json | jq '.functions[] | select(.name=="StartNode") | .complexity'`.

- [x] **CI workflow uses outdated Go version** — `.github/workflows/testnet.yml:20` — `GO_VERSION: '1.22'` while go.mod requires `go 1.25.0`. This mismatch may cause build issues or mask compatibility problems. **Remediation:** Update `.github/workflows/testnet.yml:20` to `GO_VERSION: '1.25'`.

- [ ] **Low package cohesion in integration package** — integration/ — go-stats-generator reports cohesion score 1.3 (below 2.0 threshold). The package contains test harness code (node.go), test definitions (integration_test.go), and result classification (report.go). **Remediation:** Consider splitting into `integration/harness/` (node.go), `integration/tests/` (integration_test.go), and `integration/report/` (report.go) if the package grows further.

### LOW

- [ ] **`TestResult` type suggested for relocation** — `integration/report.go:12-20` — go-stats-generator suggests moving `TestResult` to `cmd/report/main.go` for better cohesion (affinity gain +0.67). However, `TestResult` is used by both packages and its current location in `integration/` is reasonable for a shared type. **Remediation:** No action required; the refactoring suggestion has low ROI.

- [ ] **Oversized files identified** — `cmd/go-testnode/main.go` (281 lines), `integration/node.go` (201 lines) — go-stats-generator flags these as "oversized" but both are under 300 lines, which is acceptable for their scope. **Remediation:** No action required unless files grow significantly.

- [ ] **Magic number count inflated by import statements** — go-stats-generator reports 299 magic numbers, but inspection shows these are primarily import path strings (e.g., `"bufio"`, `"encoding/json"`), not actual numeric magic numbers. **Remediation:** No action required; this is a false positive from the tool's heuristics.

---

## Metrics Snapshot

| Metric | Value |
|--------|-------|
| Total Lines of Code | 568 |
| Total Functions | 21 |
| Total Methods | 19 |
| Total Structs | 20 |
| Total Packages | 2 |
| Total Files | 5 (Go only) |
| Average Function Length | 16.3 lines |
| Longest Function | main (74 lines) |
| Functions > 50 lines | 2 (5.0%) |
| Average Complexity | 5.1 |
| High Complexity (>10) | 3 functions |
| Documentation Coverage | 100.0% |
| Circular Dependencies | 0 |
| Dead Code | 5 functions (unreferenced, likely test helpers) |

**Top Complex Functions:**
1. `StartNode` — integration/node.go — complexity 12.7
2. `writeMarkdown` — cmd/report/main.go — complexity 11.9
3. `testFriendMessageInitiator` — cmd/go-testnode/main.go — complexity 11.9

---

## Validation Commands

```bash
# Run tests with race detector
go test -race ./...

# Run go vet
go vet ./...

# Regenerate metrics
go-stats-generator analyze . --skip-tests

# Verify CI locally
./scripts/build-ctoxcore.sh && \
  cd cmd/go-testnode && go build -o ../../bin/go-testnode . && cd ../.. && \
  go test -v ./integration/... -args -go-node=bin/go-testnode -c-node=bin/c-testnode
```

---

*Report generated by functional audit on 2026-04-07.*
