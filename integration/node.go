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
	Type    string `json:"type"`     // "ready"
	Impl    string `json:"impl"`     // human-readable name, e.g. "c-toxcore"
	ToxID   string `json:"tox_id"`   // 76-char hex Tox address
	DHTKey  string `json:"dht_key"`  // 64-char hex DHT public key
	ToxPort int    `json:"tox_port"` // UDP port the Tox instance is listening on
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
	Type    string `json:"type"` // "error"
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
	Cmd       string `json:"cmd"`         // "run_test"
	Feature   string `json:"feature"`     // e.g. "friend_request"
	Role      string `json:"role"`        // "initiator" | "responder"
	PeerToxID string `json:"peer_tox_id"` // Tox address of the other side (initiator only)
}

// cmdShutdown asks the node to exit cleanly.
type cmdShutdown struct {
	Cmd string `json:"cmd"` // "shutdown"
}

// TestNode manages a single test-node subprocess.
type TestNode struct {
	impl  string
	cmd   *exec.Cmd
	stdin io.WriteCloser
	mu    sync.Mutex
	lines chan string
	done  chan struct{}
	Ready nodeReady
}

// readReadyMessage waits for the "ready" message from a test node with a 5-second timeout.
// It returns an error if the message is not received, malformed, or has the wrong type.
func (n *TestNode) readReadyMessage() error {
	select {
	case line, ok := <-n.lines:
		if !ok {
			return fmt.Errorf("%s: stdout closed before ready", n.impl)
		}
		if err := json.Unmarshal([]byte(line), &n.Ready); err != nil {
			return fmt.Errorf("%s: bad ready message %q: %w", n.impl, line, err)
		}
		if n.Ready.Type != "ready" {
			return fmt.Errorf("%s: expected ready, got %q", n.impl, n.Ready.Type)
		}
		return nil
	case <-time.After(5 * time.Second):
		_ = n.cmd.Process.Kill()
		return fmt.Errorf("%s: timed out waiting for ready message", n.impl)
	}
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

	// Wait for the "ready" message.
	if err := n.readReadyMessage(); err != nil {
		return nil, err
	}

	return n, nil
}

// sendCmd serialises v as a single JSON line and writes it to the node's stdin.
func (n *TestNode) sendCmd(v interface{}) error {
	n.mu.Lock()
	defer n.mu.Unlock()
	data, err := json.Marshal(v)
	if err != nil {
		return fmt.Errorf("%s: marshal command: %w", n.impl, err)
	}
	_, err = fmt.Fprintf(n.stdin, "%s\n", data)
	if err != nil {
		return fmt.Errorf("%s: write command: %w", n.impl, err)
	}
	return nil
}

// Bootstrap instructs the node to join the DHT via the given peer.
func (n *TestNode) Bootstrap(host string, port int, dhtKey string) error {
	return n.sendCmd(cmdBootstrap{Cmd: "bootstrap", Host: host, Port: port, Key: dhtKey})
}

// parseResultLine parses a JSON line and returns the nodeResult if it's a result or error type.
// Returns nil, nil if the line is not a result/error message (should be skipped).
func (n *TestNode) parseResultLine(line, feature string) (*nodeResult, error) {
	var envelope struct {
		Type string `json:"type"`
	}
	if err := json.Unmarshal([]byte(line), &envelope); err != nil {
		return nil, nil // Skip malformed lines
	}

	switch envelope.Type {
	case "result":
		var result nodeResult
		if err := json.Unmarshal([]byte(line), &result); err != nil {
			return nil, fmt.Errorf("%s: bad result message: %w", n.impl, err)
		}
		return &result, nil
	case "error":
		var errMsg nodeError
		_ = json.Unmarshal([]byte(line), &errMsg)
		return &nodeResult{
			Type:     "result",
			Feature:  feature,
			Status:   "error",
			ExitCode: 1,
			Details:  errMsg.Message,
		}, nil
	default:
		return nil, nil // Skip other message types
	}
}

// waitForResult reads from the lines channel until a result is received or timeout occurs.
func (n *TestNode) waitForResult(feature string, timeout time.Duration) (*nodeResult, error) {
	deadline := time.After(timeout)
	for {
		select {
		case line, ok := <-n.lines:
			if !ok {
				return nil, fmt.Errorf("%s: stdout closed while waiting for result", n.impl)
			}
			result, err := n.parseResultLine(line, feature)
			if err != nil {
				return nil, err
			}
			if result != nil {
				return result, nil
			}
			// Continue waiting if line was not a result/error
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

// RunTest sends a run_test command and waits for the result (up to timeout).
// role must be "initiator" or "responder".
func (n *TestNode) RunTest(feature, role, peerToxID string, timeout time.Duration) (*nodeResult, error) {
	if err := n.sendCmd(cmdRunTest{
		Cmd:       "run_test",
		Feature:   feature,
		Role:      role,
		PeerToxID: peerToxID,
	}); err != nil {
		return nil, fmt.Errorf("%s: send run_test command: %w", n.impl, err)
	}

	return n.waitForResult(feature, timeout)
}

// Close sends a shutdown command and waits for the subprocess to exit.
// The shutdown command is sent first so the child process can flush its stdout
// before exiting; the done channel is closed after Wait returns so the
// stdout-reader goroutine keeps draining the pipe and prevents a deadlock.
func (n *TestNode) Close() error {
	sendErr := n.sendCmd(cmdShutdown{Cmd: "shutdown"})
	waitErr := n.cmd.Wait()
	close(n.done)
	if sendErr != nil {
		return fmt.Errorf("%s: send shutdown: %w", n.impl, sendErr)
	}
	if waitErr != nil {
		return fmt.Errorf("%s: wait for exit: %w", n.impl, waitErr)
	}
	return nil
}
