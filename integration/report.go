package integration

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"testing"
)

// TestResult is the canonical record for a single (feature, impl_a, impl_b) test run.
// It matches the JSON report schema defined in cmd/report/main.go.
type TestResult struct {
	Feature  string `json:"feature"`   // e.g. "friend_request"
	ImplA    string `json:"impl_a"`    // e.g. "go-toxcore"
	ImplB    string `json:"impl_b"`    // e.g. "c-toxcore"
	Status   string `json:"status"`    // "compatible" | "conflicting" | "not_implemented"
	ExitCode int    `json:"exit_code"` // exit code of the initiator side
	Details  string `json:"details"`   // human-readable notes
}

// classifyResults maps raw node result statuses onto the canonical tri-state.
//
// Classification rules (purely mechanical, based on exit codes):
//
//	Both sides exit_code == 0          → "compatible"
//	Both sides participate, any non-0  → "conflicting"
//	Either side reports not_implemented → "not_implemented"
//	Timeout or IPC error               → "conflicting" (exit code 3 or 1 respectively)
func classifyResults(feature, implA, implB string, resA, resB *nodeResult) TestResult {
	// Prefer the initiator's (A's) exit code as the representative.
	exitCode := resA.ExitCode

	var status, details string

	switch {
	case resA.Status == "not_implemented" || resB.Status == "not_implemented":
		status = "not_implemented"
		details = fmt.Sprintf("A: %s | B: %s", resA.Details, resB.Details)

	case resA.Status == "timeout" || resB.Status == "timeout":
		status = "conflicting"
		exitCode = 3
		details = fmt.Sprintf("A: %s | B: %s", resA.Details, resB.Details)

	case resA.Status == "error" || resB.Status == "error":
		status = "conflicting"
		exitCode = 1
		details = fmt.Sprintf("A: %s | B: %s", resA.Details, resB.Details)

	case resA.ExitCode == 0 && resB.ExitCode == 0:
		status = "compatible"
		details = resA.Details

	default:
		status = "conflicting"
		details = fmt.Sprintf("A(exit=%d): %s | B(exit=%d): %s",
			resA.ExitCode, resA.Details, resB.ExitCode, resB.Details)
	}

	return TestResult{
		Feature:  feature,
		ImplA:    implA,
		ImplB:    implB,
		Status:   status,
		ExitCode: exitCode,
		Details:  details,
	}
}

// resultsFile returns the path where intermediate JSON results are written.
// If TESTNET_RESULTS is set in the environment it is used; otherwise a
// well-known path inside the OS temp directory is returned.
func resultsFile() string {
	if p := os.Getenv("TESTNET_RESULTS"); p != "" {
		return p
	}
	return filepath.Join(os.TempDir(), "toxcore-testnet-results.json")
}

// writeResults serialises the accumulated TestResult slice to a JSON file so
// that the report generator can consume it independently of go test output.
// Failures are fatal because a missing/partial results file can silently break
// downstream CI report generation.
func writeResults(t *testing.T, results []TestResult) {
	t.Helper()
	path := resultsFile()
	data, err := json.MarshalIndent(results, "", "  ")
	if err != nil {
		t.Fatalf("could not marshal results: %v", err)
	}
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatalf("could not write results to %s: %v", path, err)
	}
	t.Logf("intermediate results written to %s", path)
}
