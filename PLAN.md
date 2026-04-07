# Implementation Plan: Fix go↔c Connectivity & Expand Feature Coverage

## Project Context
- **What it does**: A compatibility testnet that spawns c-toxcore, go-toxcore, and tox-rs as subprocesses, coordinates them via JSON-line IPC, and classifies feature compatibility as "compatible", "conflicting", or "not_implemented".
- **Current goal**: Fix the go-toxcore ↔ c-toxcore connectivity failure blocking CI, then expand feature coverage from 3/6 to 6/6 tested features.
- **Estimated Scope**: Medium (5–15 items above threshold)

## Goal-Achievement Status
| Stated Goal | Current Status | This Plan Addresses |
|-------------|---------------|---------------------|
| go↔c cross-implementation testing | ❌ Failing (22+ CI failures) | Yes — Priority 1 |
| Test dht_bootstrap feature | ✅ Implemented | No |
| Test friend_request feature | ✅ Implemented | No |
| Test friend_message feature | ✅ Implemented | No |
| Test file_transfer feature | ⚠️ Stubbed | Yes — Priority 2 |
| Test conference_invite feature | ⚠️ Stubbed | Yes — Priority 3 |
| Test conference_message feature | ⚠️ Stubbed | Yes — Priority 3 |
| Support tox-rs (Rust) node | ❌ Non-functional | No (blocked by upstream) |
| Nightly CI detects drift | ⚠️ Partial (always fails) | Yes — Priority 1 |
| README accuracy | ⚠️ Missing status column | Yes — Priority 4 |

## Metrics Summary
- **Complexity hotspots on goal-critical paths**: 4 functions above threshold (complexity > 9.0)
  - `StartNode` (12.7) — node lifecycle management
  - `testFriendMessageInitiator` (11.9) — friend message test
  - `waitForResult` (11.9) — IPC result parsing
  - `testFriendRequest` (10.6) — friend request test
- **Duplication ratio**: ~3% (IPC types duplicated across harness and go-testnode)
- **Doc coverage**: 100.0% (all exported symbols documented)
- **Package coupling**: `integration` package has low cohesion (1.3); contains harness, tests, and reporting in one package

## Implementation Steps

### Step 1: Fix go-testnode UDP Port Reporting
- **Deliverable**: Update `cmd/go-testnode/main.go` to either:
  1. Query actual bound port via opd-ai/toxcore API if available, OR
  2. Fail fast if fixed port binding fails with clear error message
- **Dependencies**: None
- **Goal Impact**: Unblocks go↔c connectivity (PRIMARY BLOCKER for CI)
- **Acceptance**: 
  - go-testnode reports the actual UDP port it's listening on
  - `netstat -ulnp | grep go-testnode` matches the port in the ready message
  - CI run passes for go↔c friend_request test
- **Validation**: 
  ```bash
  # Build and verify port binding
  cd cmd/go-testnode && go build -o ../../bin/go-testnode . && cd ../..
  timeout 5 bin/go-testnode 2>/dev/null | head -1 | jq -r '.tox_port'
  # Should output the actual bound port (33445 or fail with error)
  ```

### Step 2: Add Port Binding Failure Diagnostics
- **Deliverable**: Update `cmd/go-testnode/main.go` to log to stderr:
  - Attempted port range on startup
  - Actual bound port (if discoverable)
  - Clear error if port 33445 is unavailable
- **Dependencies**: Step 1
- **Goal Impact**: Improves debugging for future connectivity issues
- **Acceptance**: stderr shows diagnostic line like `go-testnode: bound to UDP port 33445`
- **Validation**:
  ```bash
  bin/go-testnode 2>&1 | grep -E "bound to UDP port|port.*unavailable"
  ```

### Step 3: Implement file_transfer in c-testnode
- **Deliverable**: Update `testnode/ctoxcore/main.c` to implement `test_file_transfer`:
  - Initiator: call `tox_file_send()` with 1KB test blob, handle `file_chunk_request` callback
  - Responder: accept via `tox_file_control()`, receive via `file_recv_chunk` callback, verify blob integrity
  - Both: emit result with "compatible" if blob hash matches, "conflicting" otherwise
- **Dependencies**: Step 1 (CI must pass for c↔c to test file transfer)
- **Goal Impact**: Expands feature coverage from 3/6 to 4/6
- **Acceptance**: 
  - `file_transfer` test between c-testnode ↔ c-testnode shows "compatible"
  - 1KB blob transferred and verified
- **Validation**:
  ```bash
  go test -v ./integration/... -run TestFileTransfer -args -c-node=bin/c-testnode
  # Expected: PASS with status "compatible" for c↔c pair
  ```

### Step 4: Implement file_transfer in go-testnode
- **Deliverable**: Update `cmd/go-testnode/main.go` to implement `testFileTransfer`:
  - Check if opd-ai/toxcore exposes file transfer API
  - If available: implement matching protocol to c-testnode
  - If not available: keep returning "not_implemented" with updated message
- **Dependencies**: Step 3 (c-testnode implementation provides reference)
- **Goal Impact**: Enables go↔c file transfer testing
- **Acceptance**:
  - `file_transfer` test between go-testnode ↔ c-testnode shows "compatible" or accurate "not_implemented"
- **Validation**:
  ```bash
  # Check API availability
  go doc github.com/opd-ai/toxcore | grep -i file
  # Then run test
  go test -v ./integration/... -run TestFileTransfer -args -go-node=bin/go-testnode -c-node=bin/c-testnode
  ```

### Step 5: Implement conference_invite in c-testnode
- **Deliverable**: Update `testnode/ctoxcore/main.c` to implement `test_conference_invite`:
  - Initiator: `tox_conference_new()`, `tox_conference_invite()`
  - Responder: accept via `conference_invite` callback, `tox_conference_join()`
  - Both: verify membership via `tox_conference_peer_count()`
- **Dependencies**: Step 3 (establishes callback pattern)
- **Goal Impact**: Expands feature coverage to 5/6
- **Acceptance**: 
  - `conference_invite` test between c-testnode ↔ c-testnode shows "compatible"
- **Validation**:
  ```bash
  go test -v ./integration/... -run TestConferenceInvite -args -c-node=bin/c-testnode
  ```

### Step 6: Implement conference_message in c-testnode
- **Deliverable**: Update `testnode/ctoxcore/main.c` to implement `test_conference_message`:
  - Both nodes join conference (via Step 5 mechanism)
  - Initiator: `tox_conference_send_message()`
  - Responder: verify receipt via `conference_message` callback
- **Dependencies**: Step 5
- **Goal Impact**: Expands feature coverage to 6/6
- **Acceptance**:
  - `conference_message` test between c-testnode ↔ c-testnode shows "compatible"
- **Validation**:
  ```bash
  go test -v ./integration/... -run TestConferenceMessage -args -c-node=bin/c-testnode
  ```

### Step 7: Update README Feature Status Table
- **Deliverable**: Update `README.md` lines 104-112 to add "Status" column:
  ```markdown
  | Feature | Description | Status |
  |---------|-------------|--------|
  | `dht_bootstrap` | DHT connectivity | ✅ Implemented |
  | `friend_request` | Send/accept friend requests | ✅ Implemented |
  | `friend_message` | Exchange text messages | ✅ Implemented |
  | `file_transfer` | Transfer binary blobs | 🚧 Planned / ✅ Implemented |
  | `conference_invite` | Group invitations | 🚧 Planned / ✅ Implemented |
  | `conference_message` | Group messaging | 🚧 Planned / ✅ Implemented |
  ```
- **Dependencies**: Steps 3–6 (update status as each is implemented)
- **Goal Impact**: README accuracy — users understand actual capability
- **Acceptance**: README feature table has accurate status for all 6 features
- **Validation**:
  ```bash
  grep -A 8 "| Feature |" README.md | grep -c "✅\|🚧"
  # Expected: 6 (one status indicator per feature)
  ```

### Step 8: Fix CI Go Version Mismatch
- **Deliverable**: Update `.github/workflows/testnet.yml` line 20 from `GO_VERSION: '1.22'` to `GO_VERSION: '1.25'`
- **Dependencies**: None (can be done in parallel with other steps)
- **Goal Impact**: CI builds with same Go version as go.mod requires
- **Acceptance**: CI workflow uses Go 1.25
- **Validation**:
  ```bash
  grep "GO_VERSION" .github/workflows/testnet.yml
  # Expected: GO_VERSION: '1.25'
  ```

---

## Validation Plan

After all steps complete:

```bash
# Full integration test suite
./scripts/build-ctoxcore.sh
cd cmd/go-testnode && go build -o ../../bin/go-testnode . && cd ../..
go test -v ./integration/... -args -go-node=bin/go-testnode -c-node=bin/c-testnode

# Verify metrics improvement
go-stats-generator analyze . --skip-tests --format json | jq '.overview'

# Expected outcomes:
# - CI passes (no timeout on go↔c tests)
# - file_transfer shows "compatible" for c↔c, accurate status for go↔c
# - conference_invite shows "compatible" for c↔c
# - conference_message shows "compatible" for c↔c
# - README accurately reflects implementation status
```

---

## Deferred Work (Not in This Plan)

| Item | Reason |
|------|--------|
| Restore Rust test node | Blocked by tox-rs upstream (no high-level API) |
| Extract shared protocol package | Low priority per ROADMAP.md; IPC types work correctly |
| Reduce StartNode complexity | Code is functional; refactoring has low ROI |
| Split integration package | Package is small (201 LOC); splitting premature |

---

*Generated 2026-04-07 by go-stats-generator metrics analysis + project goal synthesis*
