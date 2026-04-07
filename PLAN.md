# Implementation Plan: Cross-Implementation Testnet Build Fix

## Project Context
- **What it does**: A compatibility testnet for c-toxcore, go-toxcore (opd-ai/toxcore), and tox-rs that spawns each implementation as a subprocess, coordinates them over JSON-line IPC protocol, and classifies feature compatibility.
- **Current goal**: Fix all three test node builds so the compatibility testnet can produce meaningful results
- **Estimated Scope**: Medium (5-15 items requiring fixes across 3 codebases)

## Goal-Achievement Status
| Stated Goal | Current Status | This Plan Addresses |
|-------------|---------------|---------------------|
| JSON-line IPC protocol for test nodes | ✅ Achieved | No |
| Classification of compatibility results | ✅ Achieved | No |
| Deterministic report generation | ✅ Achieved | No |
| Nightly CI for upstream drift | ✅ Achieved | No |
| Support c-toxcore (TokTok/c-toxcore) | ❌ Missing | **Yes** |
| Support go-toxcore (opd-ai/toxcore) | ❌ Missing | **Yes** |
| Support tox-rs/tox | ❌ Missing | **Yes** |
| Test DHT bootstrap | ⚠️ Blocked | Indirectly (enables testing) |
| Test friend request | ⚠️ Blocked | Indirectly (enables testing) |
| Test friend message | ⚠️ Blocked | Indirectly (enables testing) |
| Test file transfer | ⚠️ Stubbed | No (future work) |
| Test conference invite | ⚠️ Stubbed | No (future work) |
| Test conference message | ⚠️ Stubbed | No (future work) |

## Metrics Summary
- **Complexity hotspots on goal-critical paths**: 3 functions above threshold (cyclomatic >10)
  - `StartNode` (integration/node.go): 14
  - `main` (cmd/go-testnode/main.go): 12
  - `testFriendMessage` (cmd/go-testnode/main.go): 11
- **Duplication ratio**: 0% (no code clones detected)
- **Doc coverage**: 100% (all exported symbols documented)
- **Package coupling**: Low (0.5 coupling score for main package)
- **Anti-patterns detected**: 7 bare error returns (high severity), 1 goroutine leak risk, 3 memory allocation inefficiencies

## Implementation Steps

### Step 1: Fix C Test Node Build — Initialize Git Submodules
- **Deliverable**: Update `scripts/build-ctoxcore.sh` to initialize git submodules when cloning TokTok/c-toxcore, resolving the "Cannot find source file: third_party/cmp/cmp.c" CMake error
- **Dependencies**: None
- **Goal Impact**: Enables c-toxcore support — the reference implementation required for all compatibility testing
- **Acceptance**: `bash scripts/build-ctoxcore.sh` completes without CMake errors; `bin/c-testnode` binary exists
- **Validation**: 
  ```bash
  bash scripts/build-ctoxcore.sh && test -x bin/c-testnode && echo "PASS"
  ```

### Step 2: Fix Go Test Node API Compatibility
- **Deliverable**: Update `cmd/go-testnode/main.go` to use correct opd-ai/toxcore API methods:
  - Replace `n.tox.IsConnected` with `n.tox.IsConnected()` (method call)
  - Replace `n.tox.FriendAdd` with `n.tox.AddFriend` or `n.tox.AddFriendByPublicKey`
  - Replace `n.tox.FriendIsConnected` with `n.tox.FriendGetConnectionStatus`
  - Replace `n.tox.SetFriendMessageCallback` with `n.tox.OnFriendMessage`
  - Fix `FriendSendMessage` return value handling (returns `(uint32, error)`)
  - Fix `MessageTypeNormal` type usage
- **Dependencies**: None (can be done in parallel with Step 1)
- **Goal Impact**: Enables go-toxcore support — allows testing 2/3 of implementation pairs (go↔c, go↔rust)
- **Acceptance**: `cd cmd/go-testnode && go build .` succeeds without compilation errors
- **Validation**: 
  ```bash
  cd cmd/go-testnode && go build -o /dev/null . && echo "PASS"
  ```

### Step 3: Fix Rust Test Node API Compatibility
- **Deliverable**: Update `testnode/tox_rs/src/main.rs` and `Cargo.toml` to use current tox-rs crate structure:
  - Replace `tox::toxcore::*` imports with correct paths from `tox_core`, `tox_crypto`, `tox_packet` crates
  - Update `Node` struct to use current API types
  - If high-level Tox struct unavailable in tox-rs, either:
    - Implement minimal wrapper using low-level primitives, or
    - Mark Rust node as optional in CI and document limitation
- **Dependencies**: None (can be done in parallel with Steps 1-2)
- **Goal Impact**: Enables tox-rs support — allows testing all 3/3 implementation pairs
- **Acceptance**: `cd testnode/tox_rs && cargo build --release` succeeds
- **Validation**: 
  ```bash
  cd testnode/tox_rs && cargo build --release 2>&1 | grep -q "Compiling" && test -x target/release/rust-testnode && echo "PASS"
  ```

### Step 4: Verify CI Pipeline Passes
- **Deliverable**: Push fixes from Steps 1-3 to main branch; verify GitHub Actions workflow "Toxcore Cross-Implementation Testnet" passes
- **Dependencies**: Steps 1, 2, 3 (all builds must succeed)
- **Goal Impact**: Enables nightly CI drift detection and compatibility report generation
- **Acceptance**: Workflow conclusion is "success" and compatibility-report.json/md artifacts are downloadable
- **Validation**: 
  ```bash
  gh run list --workflow=testnet.yml --limit=1 --json conclusion -q '.[0].conclusion' | grep -q "success" && echo "PASS"
  ```

### Step 5: Reduce Complexity in StartNode Function
- **Deliverable**: Extract `readReadyMessage()` helper from `integration/node.go:StartNode()` to handle the ready-message parsing loop (lines ~113-128), reducing cyclomatic complexity from 14 to ≤10
- **Dependencies**: Step 4 (CI passing confirms test harness works correctly before refactoring)
- **Goal Impact**: Improves code maintainability; makes future protocol changes safer
- **Acceptance**: `StartNode` cyclomatic complexity ≤10; all tests pass
- **Validation**: 
  ```bash
  go-stats-generator analyze . --skip-tests --format json --sections functions 2>/dev/null | jq '.functions[] | select(.name=="StartNode") | .complexity.cyclomatic' | grep -E '^([0-9]|10)$' && go test ./integration/... && echo "PASS"
  ```

### Step 6: Reduce Complexity in go-testnode main Function
- **Deliverable**: Extract `dispatchCommand()` helper from `cmd/go-testnode/main.go:main()` to handle the switch statement on command type, reducing cyclomatic complexity from 12 to ≤10
- **Dependencies**: Step 2 (Go node must compile first)
- **Goal Impact**: Improves code maintainability; makes adding new test features easier
- **Acceptance**: `main` function cyclomatic complexity ≤10; go-testnode builds successfully
- **Validation**: 
  ```bash
  go-stats-generator analyze . --skip-tests --format json --sections functions 2>/dev/null | jq '.functions[] | select(.name=="main" and .file=="cmd/go-testnode/main.go") | .complexity.cyclomatic' | grep -E '^([0-9]|10)$' && echo "PASS"
  ```

### Step 7: Add Error Context Wrapping
- **Deliverable**: Wrap bare error returns in `integration/node.go` (lines 138, 153, 213) and `cmd/report/main.go` (lines 179, 196, 270) with `fmt.Errorf("context: %w", err)` to preserve error chains and improve debugging
- **Dependencies**: Steps 4-6 (refactoring should be complete before error handling changes)
- **Goal Impact**: Improves debugging experience when test failures occur
- **Acceptance**: `go-stats-generator` reports 0 bare_error_return anti-patterns in affected files
- **Validation**: 
  ```bash
  go-stats-generator analyze . --skip-tests --format json --sections patterns 2>/dev/null | jq '[.patterns.anti_patterns.performance_antipatterns[] | select(.type=="bare_error_return")] | length' | grep -q "^0$" && echo "PASS"
  ```

### Step 8: Expand README Documentation
- **Deliverable**: Update `README.md` with:
  - Prerequisites (Go 1.22+, Rust stable, CMake, Ninja, libsodium-dev, pkg-config)
  - Build instructions for each test node
  - How to run tests locally with example commands
  - How to generate and interpret reports
  - Architecture overview of JSON-line IPC protocol
- **Dependencies**: Steps 1-4 (documentation should reflect working build process)
- **Goal Impact**: Enables new contributors to onboard and run the testnet locally
- **Acceptance**: README contains ≥500 words and sections for Prerequisites, Building, Running Tests, Reports, Architecture
- **Validation**: 
  ```bash
  wc -w README.md | awk '{if ($1 >= 500) print "PASS"; else print "FAIL"}'
  ```

---

## Priority Order Rationale

1. **Steps 1-3** (Build fixes) are **CRITICAL** — nothing works until all three implementations compile. These can be done in parallel.
2. **Step 4** (CI verification) confirms the fixes work end-to-end in the automated environment.
3. **Steps 5-7** (Code quality) are **LOW** priority — they improve maintainability but don't block functionality. The codebase is already well-documented (100% coverage) with no duplication.
4. **Step 8** (Documentation) supports contributor onboarding but isn't blocking for the testnet's primary users (the maintainers).

## Scope Assessment Calibration

| Metric | This Project | Threshold | Assessment |
|--------|--------------|-----------|------------|
| Functions above complexity 9.0 | 5 | 5-15 = Medium | Medium |
| Duplication ratio | 0% | <3% = Small | Small |
| Doc coverage gap | 0% | <10% = Small | Small |
| Build failures to fix | 3 | — | Medium |

**Overall: Medium** — The primary work is fixing 3 build failures across different languages/toolchains, which requires research into upstream API changes but involves relatively small code changes.

---

*Generated on 2026-04-07 using go-stats-generator v1.0.0 metrics.*
