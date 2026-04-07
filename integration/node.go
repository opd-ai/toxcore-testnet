package integration

import (
	"bufio"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"sync"
	"time"
)

// nodeReady is the first line a test node writes to stdout once it is
// initialised and ready to accept commands.
type nodeReady struct {
	Type        string `json:"type"`         // "ready"
	Impl        string `json:"impl"`         // human-readable name, e.g. "c-toxcore"
	ToxID       string `json:"tox_id"`       // 76-char hex Tox address
	DHTKey      string `json:"dht_key"`      // 64-char hex DHT public key
	ToxPort     int    `json:"tox_port"`     // UDP port the Tox instance is listening on
}

// nodeResult is emitted after a test finishes.
type nodeResult struct {
	Type     string `json:"type"`      // "result"
	Feature  string `json:"feature"`   // e.g. "friend_request"
	Status   string `json:"status"`    // "compatible" | "conflicting" | "not_implemented" | "timeout" | "error"
	ExitCode int    `json:"exit_code"` // 0 = pass, 1 = fail, 2 = not_implemented, 3 = timeout
	Details  string `json:"details"`   // human-readable description
}

// nodeError is emitted when a node encounters an unrecoverable error.
type nodeError struct {
	Type    string `json:"type"`    // "error"
	Message string `json:"message"`
}

// cmdBootstrap asks a node to connect to the DHT via a known peer.
type cmdBootstrap struct {
	Cmd  string `json:"cmd"`  // "bootstrap"
	Host string `json:"host"` // "127.0.0.1"
	Port int    `json:"port"`
	Key  string `json:"key"` // 64-char hex DHT public key
}

// cmdRunTest instructs a node to execute one test case.
type cmdRunTest struct {
	Cmd        string `json:"cmd"`          // "run_test"
	Feature    string `json:"feature"`      // e.g. "friend_request"
	Role       string `json:"role"`         // "initiator" | "responder"
	PeerToxID  string `json:"peer_tox_id"`  // Tox address of the other side (initiator only)
}

// cmdShutdown asks the node to exit cleanly.
type cmdShutdown struct {
	Cmd string `json:"cmd"` // "shutdown"
}

// TestNode manages a single test-node subprocess.
type TestNode struct {
	impl   string
	cmd    *exec.Cmd
	stdin  io.WriteCloser
	mu     sync.Mutex
	lines  chan string
	done   chan struct{}
	Ready  nodeReady
}

// StartNode launches the binary at binaryPath, reads its "ready" message, and
// returns a TestNode ready to accept commands. implName is the human-readable
// label used in reports (e.g. "go-toxcore").
func StartNode(binaryPath, implName string) (*TestNode, error) {
	cmd := exec.Command(binaryPath)
	cmd.Stderr = os.Stderr

	stdin, err := cmd.StdinPipe()
	if err != nil {
		return nil, fmt.Errorf("stdin pipe for %s: %w", implName, err)
	}

	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return nil, fmt.Errorf("stdout pipe for %s: %w", implName, err)
	}

	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("start %s: %w", implName, err)
	}

	n := &TestNode{
		impl:  implName,
		cmd:   cmd,
		stdin: stdin,
		lines: make(chan string, 256),
		done:  make(chan struct{}),
	}

	// Read all stdout lines into a buffered channel.
	go func() {
		scanner := bufio.NewScanner(stdout)
		for scanner.Scan() {
			select {
			case n.lines <- scanner.Text():
			case <-n.done:
				return
			}
		}
		close(n.lines)
	}()

	// Wait for the "ready" message (5-second timeout).
	select {
	case line, ok := <-n.lines:
		if !ok {
			return nil, fmt.Errorf("%s: stdout closed before ready", implName)
		}
		if err := json.Unmarshal([]byte(line), &n.Ready); err != nil {
			return nil, fmt.Errorf("%s: bad ready message %q: %w", implName, line, err)
		}
		if n.Ready.Type != "ready" {
			return nil, fmt.Errorf("%s: expected ready, got %q", implName, n.Ready.Type)
		}
	case <-time.After(5 * time.Second):
		_ = n.cmd.Process.Kill()
		return nil, fmt.Errorf("%s: timed out waiting for ready message", implName)
	}

	return n, nil
}

// sendCmd serialises v as a single JSON line and writes it to the node's stdin.
func (n *TestNode) sendCmd(v interface{}) error {
	n.mu.Lock()
	defer n.mu.Unlock()
	data, err := json.Marshal(v)
	if err != nil {
		return err
	}
	_, err = fmt.Fprintf(n.stdin, "%s\n", data)
	return err
}

// Bootstrap instructs the node to join the DHT via the given peer.
func (n *TestNode) Bootstrap(host string, port int, dhtKey string) error {
	return n.sendCmd(cmdBootstrap{Cmd: "bootstrap", Host: host, Port: port, Key: dhtKey})
}

// RunTest sends a run_test command and waits for the result (up to timeout).
// role must be "initiator" or "responder".
func (n *TestNode) RunTest(feature, role, peerToxID string, timeout time.Duration) (*nodeResult, error) {
	if err := n.sendCmd(cmdRunTest{
		Cmd:       "run_test",
		Feature:   feature,
		Role:      role,
		PeerToxID: peerToxID,
	}); err != nil {
		return nil, err
	}

	deadline := time.After(timeout)
	for {
		select {
		case line, ok := <-n.lines:
			if !ok {
				return nil, fmt.Errorf("%s: stdout closed while waiting for result", n.impl)
			}
			var envelope struct {
				Type string `json:"type"`
			}
			if err := json.Unmarshal([]byte(line), &envelope); err != nil {
				continue
			}
			switch envelope.Type {
			case "result":
				var r nodeResult
				if err := json.Unmarshal([]byte(line), &r); err != nil {
					return nil, fmt.Errorf("%s: bad result message: %w", n.impl, err)
				}
				return &r, nil
			case "error":
				var e nodeError
				_ = json.Unmarshal([]byte(line), &e)
				return &nodeResult{
					Type:     "result",
					Feature:  feature,
					Status:   "error",
					ExitCode: 1,
					Details:  e.Message,
				}, nil
			}
		case <-deadline:
			return &nodeResult{
				Type:     "result",
				Feature:  feature,
				Status:   "timeout",
				ExitCode: 3,
				Details:  fmt.Sprintf("%s did not respond within %s", n.impl, timeout),
			}, nil
		}
	}
}

// Close sends a shutdown command and waits for the subprocess to exit.
func (n *TestNode) Close() error {
	close(n.done)
	_ = n.sendCmd(cmdShutdown{Cmd: "shutdown"})
	return n.cmd.Wait()
}
