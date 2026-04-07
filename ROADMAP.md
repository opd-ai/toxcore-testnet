# Roadmap

## Status

Many items from the original roadmap have been completed. This document now focuses on remaining and future work.

**Completed:**
- ✅ Go test node API compatibility fixed (opd-ai/toxcore v1.4.0+)
- ✅ C test node build fixed (submodule init, pkg-config path)
- ✅ Rust test node stubbed (tox-rs API restructured, reports not_implemented)
- ✅ Code complexity reduced (StartNode, main, testFriendMessage)
- ✅ README expanded with prerequisites, build instructions, usage examples

---

## Priority 1: Implement File Transfer Test

**Impact**: Medium — expands feature coverage from 3/6 to 4/6 tested features

All three nodes explicitly return `not_implemented` for `file_transfer`.

- [ ] Audit c-toxcore file transfer API: `tox_file_send`, `tox_file_send_chunk`, callbacks
- [ ] Implement `test_file_transfer` in c-testnode with small binary blob roundtrip
- [ ] Implement corresponding test in go-testnode (after API is confirmed in opd-ai/toxcore)
- [ ] Implement corresponding test in rust-testnode (after tox-rs provides high-level API)
- [ ] Validation: run full test matrix, verify `file_transfer` shows "compatible" for c↔c pairs

## Priority 2: Implement Conference/Group Chat Tests

**Impact**: Medium — expands feature coverage to 6/6 tested features

Conference invite and message tests are stubbed as `not_implemented` in all nodes.

- [ ] Audit opd-ai/toxcore for conference API support
- [ ] Audit tox-rs/tox for conference API support
- [ ] Implement `test_conference_invite` in c-testnode
- [ ] Implement `test_conference_message` in c-testnode
- [ ] Port implementations to other nodes as APIs become available
- [ ] Validation: conference tests show "compatible" or accurate "not_implemented" per implementation

## Priority 3: Restore Full Rust Node Implementation

**Impact**: Medium — currently the Rust node stubs all tests

The tox-rs/tox library restructured its API. The old `tox::toxcore::*` module hierarchy no longer exists.

- [ ] Monitor tox-rs/tox for a high-level `Tox` struct (currently only low-level primitives)
- [ ] When available, implement full Rust test node functionality
- [ ] Alternative: consider wrapping c-toxcore via FFI in Rust if tox-rs remains low-level only

## Priority 4: Shared Protocol Package

**Impact**: Low — code organization improvement

The IPC protocol types are defined in `integration/node.go` but duplicated across test node implementations.

- [ ] Extract IPC types into a shared `protocol/` package
- [ ] Generate equivalent types for C and Rust from Go definitions
- [ ] Reduce duplication and ensure protocol consistency across implementations

---

## Notes

- The C test node (c-testnode) is the most complete implementation
- The JSON-line IPC protocol is consistently implemented across nodes
- The report generator is deterministic and avoids any AI/LLM usage
- Feature tests for file_transfer and conferences are intentionally stubbed as placeholders
