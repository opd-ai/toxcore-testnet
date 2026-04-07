# toxcore-testnet

A compatibility testnet for c-toxcore, go-toxcore (opd-ai/toxcore), and tox-rs implementations. This project spawns each Tox implementation as a subprocess, coordinates them over a JSON-line IPC protocol, and classifies feature compatibility as "compatible", "conflicting", or "not_implemented".

## Prerequisites

- **Go**: 1.25.0 or later
- **Rust**: stable toolchain (for rust-testnode)
- **CMake**: 3.16+
- **Ninja**: build system
- **libsodium-dev**: development headers
- **pkg-config**: for finding libraries

### Ubuntu / Debian

```bash
sudo apt-get update
sudo apt-get install -y cmake ninja-build libsodium-dev pkg-config
```

## Building

### C Test Node (c-testnode)

The C test node wraps TokTok/c-toxcore:

```bash
./scripts/build-ctoxcore.sh
```

This clones c-toxcore, builds it with CMake, and produces `bin/c-testnode`.

### Go Test Node (go-testnode)

The Go test node wraps opd-ai/toxcore:

```bash
cd cmd/go-testnode
go build -o ../../bin/go-testnode .
```

### Rust Test Node (rust-testnode)

The Rust test node (currently stubbed due to tox-rs API restructuring):

```bash
./scripts/build-tox-rs.sh
```

Or manually:

```bash
cd testnode/tox_rs
cargo build --release
cp target/release/rust-testnode ../../bin/
```

> ⚠️ **Rust Support Status**: The Rust test node currently reports all tests as `not_implemented` because the tox-rs library has restructured its API and no longer provides a high-level `Tox` struct. The testnet still includes the Rust node in CI for forward compatibility — once tox-rs restores high-level APIs, the node will be updated. See [ROADMAP.md](ROADMAP.md#priority-3-restore-full-rust-node-implementation) for tracking.

## Running Tests

Run the full compatibility matrix:

```bash
go test -v ./integration/... \
  -args \
  -go-node=bin/go-testnode \
  -c-node=bin/c-testnode \
  -rust-node=bin/rust-testnode
```

### Generating Reports

After tests complete, generate compatibility reports:

```bash
# Generate JSON report
go test -v -json ./integration/... > test-output.json
go run ./cmd/report -input test-output.json -json report.json -md report.md
```

## Architecture

### JSON-Line IPC Protocol

Test nodes communicate with the harness via a JSON-line protocol:

**Node → Harness (stdout)**:
```json
{"type":"ready","impl":"go-toxcore","tox_id":"HEX","dht_key":"HEX","tox_port":N}
{"type":"result","feature":"friend_request","status":"compatible","exit_code":0,"details":"..."}
{"type":"error","message":"..."}
```

**Harness → Node (stdin)**:
```json
{"cmd":"bootstrap","host":"127.0.0.1","port":N,"key":"HEX"}
{"cmd":"run_test","feature":"friend_request","role":"initiator","peer_tox_id":"HEX"}
{"cmd":"shutdown"}
```

### Test Features

| Feature | Description | Status |
|---------|-------------|--------|
| `dht_bootstrap` | DHT connectivity after bootstrapping | ✅ Implemented |
| `friend_request` | Send and accept friend requests | ✅ Implemented |
| `friend_message` | Exchange text messages between friends | ✅ Implemented |
| `file_transfer` | Transfer binary blobs | ✅ C node, 🚧 Go node |
| `conference_invite` | Group conference invitations | ✅ C node, 🚧 Go node |
| `conference_message` | Group conference messaging | ✅ C node, 🚧 Go node |

### Compatibility Status Values

- **compatible**: Both implementations behave correctly together
- **conflicting**: Incompatibility detected between implementations
- **not_implemented**: Feature not implemented in one or both nodes
- **timeout**: Test did not complete within the time limit
- **error**: An error occurred during testing

## CI/CD

The project includes a GitHub Actions workflow (`.github/workflows/testnet.yml`) that:

1. Builds all three test nodes
2. Runs the full compatibility matrix
3. Generates JSON and Markdown reports
4. Uploads reports as artifacts

The workflow runs nightly at 02:00 UTC to detect upstream implementation drift.

## License

See [LICENSE](LICENSE) for details.
