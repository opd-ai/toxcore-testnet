// Package integration provides the Toxcore cross-implementation testnet harness.
//
// It spawns each implementation as a subprocess, coordinates them over a
// JSON-line IPC protocol, and classifies each feature/pair combination as
// "compatible", "conflicting", or "not_implemented".
//
// Run with:
//
//	go test -v -json ./integration/... \
//	  -args \
//	  -go-node  /path/to/go-testnode  \
//	  -c-node   /path/to/c-testnode   \
//	  -rust-node /path/to/rust-testnode
package integration
