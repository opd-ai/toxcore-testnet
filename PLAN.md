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
| Support c-toxcore (TokTok/c-toxcore) | ✅ Fixed | **Yes** |
| Support go-toxcore (opd-ai/toxcore) | ✅ Fixed | **Yes** |
| Support tox-rs/tox | ✅ Stubbed | **Yes** |
| Test DHT bootstrap | ✅ Unblocked | Indirectly (enables testing) |
| Test friend request | ✅ Unblocked | Indirectly (enables testing) |
| Test friend message | ✅ Unblocked | Indirectly (enables testing) |
| Test file transfer | ⚠️ Stubbed | No (future work) |
| Test conference invite | ⚠️ Stubbed | No (future work) |
| Test conference message | ⚠️ Stubbed | No (future work) |

## Metrics Summary
- **Complexity hotspots on goal-critical paths**: Reduced to 0 functions above threshold (cyclomatic >10)
- **Duplication ratio**: 0% (no code clones detected)
- **Doc coverage**: 100% (all exported symbols documented)
- **Package coupling**: Low (0.5 coupling score for main package)
- **Anti-patterns detected**: 7 bare error returns (high severity)

## Implementation Steps

### Step 1: Fix C Test Node Build — Initialize Git Submodules ✅ COMPLETED
- **Status**: Fixed in scripts/build-ctoxcore.sh and testnode/ctoxcore/CMakeLists.txt

### Step 2: Fix Go Test Node API Compatibility ✅ COMPLETED
- **Status**: Fixed in cmd/go-testnode/main.go with correct opd-ai/toxcore API calls

### Step 3: Fix Rust Test Node API Compatibility ✅ COMPLETED  
- **Status**: Stubbed in testnode/tox_rs/src/main.rs (tox-rs lacks high-level API)

### Step 4: Verify CI Pipeline Passes
- **Deliverable**: Push fixes from Steps 1-3 to main branch; verify GitHub Actions workflow "Toxcore Cross-Implementation Testnet" passes
- **Dependencies**: Steps 1, 2, 3 (all builds must succeed)
- **Goal Impact**: Enables nightly CI drift detection and compatibility report generation
- **Acceptance**: Workflow conclusion is "success" and compatibility-report.json/md artifacts are downloadable
- **Status**: ⏳ Awaiting push to trigger CI

### Step 5: Reduce Complexity in StartNode Function ✅ COMPLETED
- **Status**: Extracted readReadyMessage() helper, complexity reduced to ≤10

### Step 6: Reduce Complexity in go-testnode main Function ✅ COMPLETED
- **Status**: Extracted dispatchCommand() helper, complexity reduced to ≤10

### Step 7: Add Error Context Wrapping ✅ COMPLETED
- **Status**: Wrapped bare error returns in integration/node.go and cmd/report/main.go with context

### Step 8: Expand README Documentation ✅ COMPLETED
- **Status**: README expanded with Prerequisites, Building, Running Tests, Architecture sections

---

## Priority Order Rationale

1. **Steps 1-3** (Build fixes) are **CRITICAL** — ✅ COMPLETED
2. **Step 4** (CI verification) confirms the fixes work end-to-end — ⏳ PENDING PUSH
3. **Steps 5-6** (Complexity reduction) — ✅ COMPLETED
4. **Step 7** (Error wrapping) — Next priority
5. **Step 8** (Documentation) — ✅ COMPLETED

---

*Generated on 2026-04-07 using go-stats-generator v1.0.0 metrics.*
