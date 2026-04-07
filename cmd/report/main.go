// report generates a deterministic compatibility report from go test -json
// output and/or a pre-built intermediate results JSON file.
//
// Usage:
//
//	go run ./cmd/report \
//	  -input=test-output.ndjson \
//	  -results=<path-to-results.json>  \   # optional; written by integration tests
//	  -json-out=compatibility-report.json \
//	  -md-out=compatibility-report.md
//
// The program does NOT invoke any LLM, AI model, or generative tool.
// Every classification is derived mechanically from exit codes already embedded
// in the TestResult records.
package main

import (
	"bufio"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"os"
	"sort"
	"strconv"
	"strings"
	"time"
)

// ---------------------------------------------------------------------------
// Input types
// ---------------------------------------------------------------------------

// TestResult matches the struct written by integration/report.go.
type TestResult struct {
	Feature  string `json:"feature"`
	ImplA    string `json:"impl_a"`
	ImplB    string `json:"impl_b"`
	Status   string `json:"status"`
	ExitCode int    `json:"exit_code"`
	Details  string `json:"details"`
}

// goTestEvent is one line of `go test -json` output.
type goTestEvent struct {
	Time    time.Time `json:"Time"`
	Action  string    `json:"Action"`
	Package string    `json:"Package"`
	Test    string    `json:"Test"`
	Output  string    `json:"Output"`
	Elapsed float64   `json:"Elapsed"`
}

// ---------------------------------------------------------------------------
// Flags
// ---------------------------------------------------------------------------

var (
	flagInput   = flag.String("input", "", "path to go test -json output (NDJSON)")
	flagResults = flag.String("results", "", "path to intermediate results JSON written by integration tests")
	flagJSONOut = flag.String("json-out", "compatibility-report.json", "path for the JSON artifact")
	flagMDOut   = flag.String("md-out", "compatibility-report.md", "path for the Markdown summary")
)

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

func main() {
	flag.Parse()
	log.SetFlags(0)
	log.SetPrefix("[report] ")

	ts := reportTimestamp()

	results, err := loadResults()
	if err != nil {
		log.Fatalf("loading results: %v", err)
	}

	if len(results) == 0 {
		log.Println("warning: no results found; generating empty report")
	}

	if err := writeJSON(results, ts); err != nil {
		log.Fatalf("writing JSON report: %v", err)
	}
	if err := writeMarkdown(results, ts); err != nil {
		log.Fatalf("writing Markdown report: %v", err)
	}

	log.Printf("report written to %s and %s", *flagJSONOut, *flagMDOut)
}

// reportTimestamp returns a stable RFC-3339 UTC timestamp for report headers.
//
// Precedence (most to least deterministic):
//  1. SOURCE_DATE_EPOCH env var (Unix seconds) — standard for reproducible builds.
//  2. GITHUB_RUN_STARTED_AT env var — set by GitHub Actions on every run.
//  3. time.Now() — fallback for local runs.
func reportTimestamp() string {
	if v := os.Getenv("SOURCE_DATE_EPOCH"); v != "" {
		if secs, err := strconv.ParseInt(v, 10, 64); err == nil {
			return time.Unix(secs, 0).UTC().Format(time.RFC3339)
		}
	}
	if v := os.Getenv("GITHUB_RUN_STARTED_AT"); v != "" {
		if t, err := time.Parse(time.RFC3339, v); err == nil {
			return t.UTC().Format(time.RFC3339)
		}
	}
	return time.Now().UTC().Format(time.RFC3339)
}

// ---------------------------------------------------------------------------
// Result loading
// ---------------------------------------------------------------------------

// loadResults collects TestResult records from all available sources.
// Sources are tried in this order:
//  1. The intermediate results file produced by the integration tests.
//  2. go test -json output (parses log lines that match a known prefix).
//
// Records from the results file take precedence; go test output is only used
// to fill gaps (e.g. if the process exited before flushing the file).
func loadResults() ([]TestResult, error) {
	seen := map[string]bool{} // de-dup by "feature|implA|implB"
	var results []TestResult

	add := func(r TestResult) {
		key := fmt.Sprintf("%s|%s|%s", r.Feature, r.ImplA, r.ImplB)
		if seen[key] {
			return
		}
		seen[key] = true
		results = append(results, r)
	}

	// Source 1: intermediate results JSON.
	if *flagResults != "" {
		rs, err := loadResultsJSON(*flagResults)
		if err != nil {
			log.Printf("warning: could not read results file: %v", err)
		} else {
			for _, r := range rs {
				add(r)
			}
		}
	}

	// Source 2: go test -json NDJSON.
	if *flagInput != "" {
		rs, err := parseGoTestJSON(*flagInput)
		if err != nil {
			log.Printf("warning: could not parse go test output: %v", err)
		} else {
			for _, r := range rs {
				add(r)
			}
		}
	}

	sort.Slice(results, func(i, j int) bool {
		ri, rj := results[i], results[j]
		if ri.Feature != rj.Feature {
			return ri.Feature < rj.Feature
		}
		if ri.ImplA != rj.ImplA {
			return ri.ImplA < rj.ImplA
		}
		return ri.ImplB < rj.ImplB
	})

	return results, nil
}

func loadResultsJSON(path string) ([]TestResult, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var rs []TestResult
	if err := json.Unmarshal(data, &rs); err != nil {
		return nil, fmt.Errorf("unmarshal %s: %w", path, err)
	}
	return rs, nil
}

// parseGoTestJSON extracts TestResult records embedded in log output lines of
// the form:  RESULT_JSON: {...}
//
// The integration tests emit these via t.Logf so they appear as "output"
// actions in the go test -json stream.
func parseGoTestJSON(path string) ([]TestResult, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()

	const prefix = "RESULT_JSON: "
	var results []TestResult
	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		var ev goTestEvent
		if err := json.Unmarshal(scanner.Bytes(), &ev); err != nil {
			continue
		}
		if ev.Action != "output" {
			continue
		}
		line := strings.TrimSpace(ev.Output)
		idx := strings.Index(line, prefix)
		if idx < 0 {
			continue
		}
		payload := line[idx+len(prefix):]
		var result TestResult
		if err := json.Unmarshal([]byte(payload), &result); err != nil {
			continue
		}
		results = append(results, result)
	}
	return results, scanner.Err()
}

// ---------------------------------------------------------------------------
// Output: JSON artifact
// ---------------------------------------------------------------------------

// reportArtifact is the top-level JSON artifact schema.
type reportArtifact struct {
	GeneratedAt string       `json:"generated_at"` // RFC 3339 UTC timestamp
	Results     []TestResult `json:"results"`
	Summary     summary      `json:"summary"`
}

type summary struct {
	Total          int `json:"total"`
	Compatible     int `json:"compatible"`
	Conflicting    int `json:"conflicting"`
	NotImplemented int `json:"not_implemented"`
	Other          int `json:"other"`
}

func buildSummary(results []TestResult) summary {
	summ := summary{Total: len(results)}
	for _, res := range results {
		switch res.Status {
		case "compatible":
			summ.Compatible++
		case "conflicting":
			summ.Conflicting++
		case "not_implemented":
			summ.NotImplemented++
		default:
			summ.Other++
		}
	}
	return summ
}

func writeJSON(results []TestResult, generatedAt string) error {
	artifact := reportArtifact{
		GeneratedAt: generatedAt,
		Results:     results,
		Summary:     buildSummary(results),
	}
	data, err := json.MarshalIndent(artifact, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(*flagJSONOut, data, 0o644)
}

// ---------------------------------------------------------------------------
// Output: Markdown summary
// ---------------------------------------------------------------------------

// statusEmoji maps a canonical status to a GitHub-flavoured emoji.
func statusEmoji(status string) string {
	switch status {
	case "compatible":
		return "✅"
	case "conflicting":
		return "❌"
	case "not_implemented":
		return "⚠️"
	default:
		return "❓"
	}
}

func writeMarkdown(results []TestResult, generatedAt string) error {
	var sb strings.Builder

	summ := buildSummary(results)

	sb.WriteString("# Toxcore Cross-Implementation Compatibility Report\n\n")
	sb.WriteString(fmt.Sprintf("Generated: %s\n\n", generatedAt))

	sb.WriteString("## Summary\n\n")
	sb.WriteString(fmt.Sprintf("| Metric | Count |\n|---|---|\n"))
	sb.WriteString(fmt.Sprintf("| Total tests | %d |\n", summ.Total))
	sb.WriteString(fmt.Sprintf("| ✅ Compatible | %d |\n", summ.Compatible))
	sb.WriteString(fmt.Sprintf("| ❌ Conflicting | %d |\n", summ.Conflicting))
	sb.WriteString(fmt.Sprintf("| ⚠️  Not implemented | %d |\n", summ.NotImplemented))
	if summ.Other > 0 {
		sb.WriteString(fmt.Sprintf("| ❓ Other | %d |\n", summ.Other))
	}
	sb.WriteString("\n")

	// Group by implementation pair.
	byPair := map[string][]TestResult{}
	var pairOrder []string
	for _, res := range results {
		key := fmt.Sprintf("%s ↔ %s", res.ImplA, res.ImplB)
		if _, ok := byPair[key]; !ok {
			pairOrder = append(pairOrder, key)
		}
		byPair[key] = append(byPair[key], res)
	}

	sb.WriteString("## Results by Implementation Pair\n\n")
	for _, pairKey := range pairOrder {
		rows := byPair[pairKey]
		sb.WriteString(fmt.Sprintf("### %s\n\n", pairKey))
		sb.WriteString("| Feature | Status | Details |\n|---|---|---|\n")
		for _, res := range rows {
			details := strings.ReplaceAll(res.Details, "|", "\\|")
			if len(details) > 120 {
				details = details[:117] + "..."
			}
			sb.WriteString(fmt.Sprintf("| `%s` | %s %s | %s |\n",
				res.Feature, statusEmoji(res.Status), res.Status, details))
		}
		sb.WriteString("\n")
	}

	sb.WriteString("## Legend\n\n")
	sb.WriteString("| Symbol | Meaning |\n|---|---|\n")
	sb.WriteString("| ✅ compatible | Both implementations passed the test (exit code 0 on both sides) |\n")
	sb.WriteString("| ❌ conflicting | Both implementations participate but produce incompatible results |\n")
	sb.WriteString("| ⚠️ not_implemented | One or both implementations do not support this feature |\n")
	sb.WriteString("\n> Report generated deterministically by test harness logic. ")
	sb.WriteString("No LLM, AI model, or generative tool was used.\n")

	return os.WriteFile(*flagMDOut, []byte(sb.String()), 0o644)
}
