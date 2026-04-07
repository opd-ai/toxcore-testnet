package integration

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"sync"
	"testing"
	"time"
)

// ---------------------------------------------------------------------------
// Flags (set via -args when invoking go test)
// ---------------------------------------------------------------------------

var (
	flagGoNode   = flag.String("go-node", "", "path to the Go (opd-ai/toxcore) test-node binary")
	flagCNode    = flag.String("c-node", "", "path to the C (TokTok/c-toxcore) test-node binary")
	flagRustNode = flag.String("rust-node", "", "path to the Rust (tox-rs/tox) test-node binary")
)

// TestMain parses flags before any test runs.
func TestMain(m *testing.M) {
	flag.Parse()
	os.Exit(m.Run())
}

// ---------------------------------------------------------------------------
// Test matrix definitions
// ---------------------------------------------------------------------------

// feature lists every Tox protocol feature exercised by the testnet.
// Each entry becomes a subtest name and a "feature" field in the report.
var allFeatures = []string{
	"dht_bootstrap",      // basic DHT connectivity
	"friend_request",     // send & accept a friend request
	"friend_message",     // exchange a text message between friends
	"file_transfer",      // transfer a small binary blob
	"conference_invite",  // invite a peer into a group conference
	"conference_message", // broadcast a message in a group conference
}

// implPair describes one implementation-pair to exercise.
type implPair struct {
	implA, binA string
	implB, binB string
}

// pairs builds the full 3-pair matrix from the registered flags.
// It skips a pair if either binary flag is empty.
func pairs() []implPair {
	type flag struct {
		name string
		bin  *string
	}
	nodes := []flag{
		{"go-toxcore", flagGoNode},
		{"c-toxcore", flagCNode},
		{"tox-rs", flagRustNode},
	}

	var ps []implPair
	for i := 0; i < len(nodes); i++ {
		for j := i + 1; j < len(nodes); j++ {
			a, b := nodes[i], nodes[j]
			if *a.bin == "" || *b.bin == "" {
				continue
			}
			ps = append(ps, implPair{a.name, *a.bin, b.name, *b.bin})
		}
	}
	return ps
}

// ---------------------------------------------------------------------------
// Test entry-point
// ---------------------------------------------------------------------------

// TestCompatibility is the single top-level test that drives the entire
// compatibility matrix.  It uses subtests so that each (feature, pair)
// combination appears as an individual entry in go test -json output.
func TestCompatibility(t *testing.T) {
	ps := pairs()
	if len(ps) == 0 {
		t.Skip("no test-node binaries configured; pass -go-node, -c-node, -rust-node")
	}

	var (
		mu      sync.Mutex
		results []TestResult
	)

	for _, p := range ps {
		p := p // capture
		for _, feat := range allFeatures {
			feat := feat // capture
			name := fmt.Sprintf("%s/%s_vs_%s", feat, p.implA, p.implB)

			t.Run(name, func(t *testing.T) {
				t.Parallel()
				result := runCompatTest(t, feat, p)

				// Emit a parseable log line so the report generator's NDJSON
				// fallback path can reconstruct results even if the intermediate
				// JSON file is not written (e.g. early process exit).
				resultJSON, _ := json.Marshal(result)
				t.Logf("RESULT_JSON: %s", resultJSON)

				mu.Lock()
				results = append(results, result)
				mu.Unlock()

				switch result.Status {
				case "compatible":
					t.Logf("COMPATIBLE: %s ↔ %s", p.implA, p.implB)
				case "conflicting":
					t.Errorf("CONFLICTING: %s", result.Details)
				case "not_implemented":
					t.Skipf("NOT IMPLEMENTED: %s", result.Details)
				default:
					t.Errorf("unexpected status %q: %s", result.Status, result.Details)
				}
			})
		}
	}

	// After all subtests finish, persist results for the report generator.
	t.Cleanup(func() {
		mu.Lock()
		defer mu.Unlock()
		writeResults(t, results)
	})
}

// ---------------------------------------------------------------------------
// Core test runner
// ---------------------------------------------------------------------------

// perTestTimeout is the maximum time allowed for a single (feature, pair) run.
// It must remain well under the 25-minute go test -timeout to leave headroom.
const perTestTimeout = 90 * time.Second

// runCompatTest executes the compatibility test for one (feature, implPair).
// It:
//  1. Starts both nodes.
//  2. Bootstraps node B off node A's DHT key.
//  3. Sends "responder" to B and "initiator" to A.
//  4. Collects results and classifies them.
func runCompatTest(t *testing.T, feature string, p implPair) TestResult {
	t.Helper()

	// Start node A (initiator side).
	nodeA, err := StartNode(p.binA, p.implA)
	if err != nil {
		return TestResult{
			Feature:  feature,
			ImplA:    p.implA,
			ImplB:    p.implB,
			Status:   "conflicting",
			ExitCode: 1,
			Details:  fmt.Sprintf("failed to start %s: %v", p.implA, err),
		}
	}
	defer nodeA.Close() //nolint:errcheck

	// Start node B (responder side).
	nodeB, err := StartNode(p.binB, p.implB)
	if err != nil {
		return TestResult{
			Feature:  feature,
			ImplA:    p.implA,
			ImplB:    p.implB,
			Status:   "conflicting",
			ExitCode: 1,
			Details:  fmt.Sprintf("failed to start %s: %v", p.implB, err),
		}
	}
	defer nodeB.Close() //nolint:errcheck

	// Bootstrap B off A so they share the same DHT.
	if err := nodeB.Bootstrap("127.0.0.1", nodeA.Ready.ToxPort, nodeA.Ready.DHTKey); err != nil {
		return TestResult{
			Feature:  feature,
			ImplA:    p.implA,
			ImplB:    p.implB,
			Status:   "conflicting",
			ExitCode: 1,
			Details:  fmt.Sprintf("bootstrap failed: %v", err),
		}
	}

	// Also bootstrap A off B so the connection is symmetric.
	if err := nodeA.Bootstrap("127.0.0.1", nodeB.Ready.ToxPort, nodeB.Ready.DHTKey); err != nil {
		return TestResult{
			Feature:  feature,
			ImplA:    p.implA,
			ImplB:    p.implB,
			Status:   "conflicting",
			ExitCode: 1,
			Details:  fmt.Sprintf("reverse bootstrap failed: %v", err),
		}
	}

	// Send test commands in parallel; collect results.
	type resErr struct {
		r *nodeResult
		e error
	}
	chA := make(chan resErr, 1)
	chB := make(chan resErr, 1)

	go func() {
		r, e := nodeA.RunTest(feature, "initiator", nodeB.Ready.ToxID, perTestTimeout)
		chA <- resErr{r, e}
	}()
	go func() {
		r, e := nodeB.RunTest(feature, "responder", nodeA.Ready.ToxID, perTestTimeout)
		chB <- resErr{r, e}
	}()

	rA := <-chA
	rB := <-chB

	// If either side errored at the IPC level, convert to a nodeResult.
	if rA.e != nil {
		rA.r = &nodeResult{Status: "error", ExitCode: 1, Details: rA.e.Error()}
	}
	if rB.e != nil {
		rB.r = &nodeResult{Status: "error", ExitCode: 1, Details: rB.e.Error()}
	}

	return classifyResults(feature, p.implA, p.implB, rA.r, rB.r)
}
