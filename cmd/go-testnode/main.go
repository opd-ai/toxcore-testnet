// go-testnode is the Go-side test node for the toxcore cross-implementation testnet.
//
// It wraps the opd-ai/toxcore library in the JSON-line IPC protocol expected by
// the integration test harness.
//
// Build (CI workflow sets the replace directive before building):
//
//	cd cmd/go-testnode
//	go mod edit -replace github.com/opd-ai/toxcore=../../vendor/opd-ai-toxcore
//	go mod tidy
//	go build -o ../../bin/go-testnode .
//
// Protocol
// --------
// On startup the node writes one line to stdout:
//
//	{"type":"ready","impl":"go-toxcore","tox_id":"HEX","dht_key":"HEX","tox_port":N}
//
// It then reads newline-delimited JSON commands from stdin:
//
//	{"cmd":"bootstrap","host":"127.0.0.1","port":N,"key":"HEX"}
//	{"cmd":"run_test","feature":"friend_request","role":"initiator","peer_tox_id":"HEX"}
//	{"cmd":"shutdown"}
//
// For each run_test it emits:
//
//	{"type":"result","feature":"...","status":"compatible|conflicting|not_implemented","exit_code":N,"details":"..."}
package main

import (
	"bufio"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/opd-ai/toxcore"
)

// ---------------------------------------------------------------------------
// IPC types (must match integration/node.go)
// ---------------------------------------------------------------------------

type readyMsg struct {
	Type    string `json:"type"`
	Impl    string `json:"impl"`
	ToxID   string `json:"tox_id"`
	DHTKey  string `json:"dht_key"`
	ToxPort int    `json:"tox_port"`
}

type resultMsg struct {
	Type     string `json:"type"`
	Feature  string `json:"feature"`
	Status   string `json:"status"`
	ExitCode int    `json:"exit_code"`
	Details  string `json:"details"`
}

type errMsg struct {
	Type    string `json:"type"`
	Message string `json:"message"`
}

type cmdEnvelope struct {
	Cmd string `json:"cmd"`
}

type cmdBootstrap struct {
	Host string `json:"host"`
	Port int    `json:"port"`
	Key  string `json:"key"`
}

type cmdRunTest struct {
	Feature   string `json:"feature"`
	Role      string `json:"role"`
	PeerToxID string `json:"peer_tox_id"`
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

func emit(v interface{}) {
	data, _ := json.Marshal(v)
	fmt.Printf("%s\n", data)
}

func emitError(msg string) {
	emit(errMsg{Type: "error", Message: msg})
}

func emitResult(feature, status string, exitCode int, details string) {
	emit(resultMsg{
		Type:     "result",
		Feature:  feature,
		Status:   status,
		ExitCode: exitCode,
		Details:  details,
	})
}

func hexToBytes(h string) ([]byte, error) {
	h = strings.ToLower(strings.TrimSpace(h))
	return hex.DecodeString(h)
}

func bytesToHex(b []byte) string {
	return strings.ToUpper(hex.EncodeToString(b))
}

// ---------------------------------------------------------------------------
// Node state
// ---------------------------------------------------------------------------

type node struct {
	tox            *toxcore.Tox
	friendRequests [][]byte // public keys of pending friend requests
	messages       []string // received messages (for friend_message test)
}

// ---------------------------------------------------------------------------
// Feature tests
// ---------------------------------------------------------------------------

// testDHTBootstrap verifies that the DHT layer is connected after bootstrapping.
// Both sides report compatible if they have at least one DHT peer.
func (n *node) testDHTBootstrap(_ string, _ string) (string, int, string) {
	// Give the DHT loop time to find the peer we bootstrapped against.
	deadline := time.Now().Add(30 * time.Second)
	for time.Now().Before(deadline) {
		if n.tox.IsConnected() {
			return "compatible", 0, "DHT peer found"
		}
		n.tox.Iterate()
		time.Sleep(time.Duration(n.tox.IterationInterval()) * time.Millisecond)
	}
	return "conflicting", 3, "DHT bootstrap timed out after 30s"
}

// testFriendRequest: initiator sends a friend request to responder's Tox ID;
// responder auto-accepts via callback.
func (n *node) testFriendRequest(role, peerToxID string) (string, int, string) {
	switch role {
	case "initiator":
		peerAddr, err := hexToBytes(peerToxID)
		if err != nil {
			return "conflicting", 1, fmt.Sprintf("bad peer address hex: %v", err)
		}
		_, err = n.tox.FriendAdd(peerAddr, "testnet-friend-request")
		if err != nil {
			return "conflicting", 1, fmt.Sprintf("FriendAdd failed: %v", err)
		}
		// Wait for the friend to come online.
		deadline := time.Now().Add(60 * time.Second)
		for time.Now().Before(deadline) {
			if n.tox.FriendIsConnected(0) {
				return "compatible", 0, "friend request accepted and friend is online"
			}
			n.tox.Iterate()
			time.Sleep(time.Duration(n.tox.IterationInterval()) * time.Millisecond)
		}
		return "conflicting", 3, "friend never came online within 60s"

	case "responder":
		// Accept friend requests automatically via the callback set in main.
		deadline := time.Now().Add(60 * time.Second)
		for time.Now().Before(deadline) {
			if len(n.friendRequests) > 0 {
				return "compatible", 0, "friend request received and accepted"
			}
			n.tox.Iterate()
			time.Sleep(time.Duration(n.tox.IterationInterval()) * time.Millisecond)
		}
		return "conflicting", 3, "no friend request received within 60s"

	default:
		return "not_implemented", 2, fmt.Sprintf("unknown role %q", role)
	}
}

// testFriendMessage: after friends are connected, initiator sends a message
// and waits for an echo; responder echoes every received message.
func (n *node) testFriendMessage(role, peerToxID string) (string, int, string) {
	const testMsg = "toxcore-testnet-ping"
	switch role {
	case "initiator":
		// Ensure we are friends first (may already be from testFriendRequest).
		peerAddr, err := hexToBytes(peerToxID)
		if err != nil {
			return "conflicting", 1, fmt.Sprintf("bad peer address hex: %v", err)
		}
		friendNum, _ := n.tox.FriendAdd(peerAddr, "testnet")
		// Wait for friend to be online.
		deadline := time.Now().Add(60 * time.Second)
		for time.Now().Before(deadline) {
			if n.tox.FriendIsConnected(friendNum) {
				break
			}
			n.tox.Iterate()
			time.Sleep(time.Duration(n.tox.IterationInterval()) * time.Millisecond)
		}
		if !n.tox.FriendIsConnected(friendNum) {
			return "conflicting", 3, "friend never came online"
		}
		if err := n.tox.FriendSendMessage(friendNum, toxcore.MessageTypeNormal, testMsg); err != nil {
			return "conflicting", 1, fmt.Sprintf("FriendSendMessage failed: %v", err)
		}
		// Wait for echo.
		deadline = time.Now().Add(30 * time.Second)
		for time.Now().Before(deadline) {
			for _, m := range n.messages {
				if m == testMsg {
					return "compatible", 0, "message sent and echo received"
				}
			}
			n.tox.Iterate()
			time.Sleep(time.Duration(n.tox.IterationInterval()) * time.Millisecond)
		}
		return "conflicting", 3, "echo not received within 30s"

	case "responder":
		// Echo every received message back.
		n.tox.SetFriendMessageCallback(func(friendNum uint32, msgType toxcore.MessageType, msg string) {
			_ = n.tox.FriendSendMessage(friendNum, msgType, msg)
		})
		deadline := time.Now().Add(60 * time.Second)
		for time.Now().Before(deadline) {
			n.tox.Iterate()
			time.Sleep(time.Duration(n.tox.IterationInterval()) * time.Millisecond)
		}
		return "compatible", 0, "responder ran message-echo loop"

	default:
		return "not_implemented", 2, fmt.Sprintf("unknown role %q", role)
	}
}

// testFileTransfer: initiator sends a small blob; responder accepts and
// verifies integrity.
//
// NOTE: File-transfer support in opd-ai/toxcore is tracked upstream. Until the
// FileSend API is confirmed and stabilised this test reports not_implemented so
// the report matrix entry is populated without causing a false conflicting result.
func (n *node) testFileTransfer(_, _ string) (string, int, string) {
	return "not_implemented", 2, "file transfer API not yet confirmed in opd-ai/toxcore"
}

// testConferenceInvite: initiator creates a conference and invites the responder.
//
// NOTE: Conference support in opd-ai/toxcore is tracked upstream. Until the
// ConferenceNew/ConferenceInvite API is confirmed this test reports not_implemented.
func (n *node) testConferenceInvite(_, _ string) (string, int, string) {
	return "not_implemented", 2, "conference API not yet confirmed in opd-ai/toxcore"
}

// testConferenceMessage: both sides join a shared group conference and exchange a message.
//
// NOTE: Conference API is tracked upstream; reports not_implemented until available.
func (n *node) testConferenceMessage(_, _ string) (string, int, string) {
	return "not_implemented", 2, "conference API not yet confirmed in opd-ai/toxcore"
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

func (n *node) runTest(feature, role, peerToxID string) (string, int, string) {
	switch feature {
	case "dht_bootstrap":
		return n.testDHTBootstrap(role, peerToxID)
	case "friend_request":
		return n.testFriendRequest(role, peerToxID)
	case "friend_message":
		return n.testFriendMessage(role, peerToxID)
	case "file_transfer":
		return n.testFileTransfer(role, peerToxID)
	case "conference_invite":
		return n.testConferenceInvite(role, peerToxID)
	case "conference_message":
		return n.testConferenceMessage(role, peerToxID)
	default:
		return "not_implemented", 2, fmt.Sprintf("feature %q not recognised", feature)
	}
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

func main() {
	log.SetOutput(os.Stderr)
	log.SetFlags(0)

	// Initialise Tox.
	opts := toxcore.NewOptions()
	opts.IPv6Enabled = false
	opts.UDPEnabled = true

	t, err := toxcore.New(opts)
	if err != nil {
		emitError(fmt.Sprintf("toxcore.New: %v", err))
		os.Exit(1)
	}
	defer t.Kill()

	n := &node{tox: t}

	// Auto-accept friend requests (needed for responder role).
	t.SetFriendRequestCallback(func(pubKey []byte, _ string) {
		if _, err := t.FriendAddNoRequest(pubKey); err != nil {
			log.Printf("FriendAddNoRequest: %v", err)
		}
		n.friendRequests = append(n.friendRequests, pubKey)
	})

	// Buffer incoming messages (needed for friend_message initiator echo test).
	t.SetFriendMessageCallback(func(_ uint32, _ toxcore.MessageType, msg string) {
		n.messages = append(n.messages, msg)
	})

	// Emit ready.
	addr := t.SelfGetAddress()
	dhtKey := t.SelfGetDHTID()
	port := t.SelfGetUDPPort()

	emit(readyMsg{
		Type:    "ready",
		Impl:    "go-toxcore",
		ToxID:   bytesToHex(addr),
		DHTKey:  bytesToHex(dhtKey),
		ToxPort: int(port),
	})

	// Handle SIGTERM gracefully.
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGTERM, syscall.SIGINT)
	go func() {
		<-sigCh
		os.Exit(0)
	}()

	// Command loop.
	scanner := bufio.NewScanner(os.Stdin)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" {
			continue
		}

		var env cmdEnvelope
		if err := json.Unmarshal([]byte(line), &env); err != nil {
			emitError(fmt.Sprintf("bad command JSON: %v", err))
			continue
		}

		switch env.Cmd {
		case "bootstrap":
			var c cmdBootstrap
			if err := json.Unmarshal([]byte(line), &c); err != nil {
				emitError(fmt.Sprintf("bad bootstrap command: %v", err))
				continue
			}
			keyBytes, err := hexToBytes(c.Key)
			if err != nil {
				emitError(fmt.Sprintf("bad bootstrap key hex: %v", err))
				continue
			}
			if err := t.Bootstrap(c.Host, uint16(c.Port), keyBytes); err != nil {
				emitError(fmt.Sprintf("Bootstrap: %v", err))
			}

		case "run_test":
			var c cmdRunTest
			if err := json.Unmarshal([]byte(line), &c); err != nil {
				emitError(fmt.Sprintf("bad run_test command: %v", err))
				continue
			}
			status, exitCode, details := n.runTest(c.Feature, c.Role, c.PeerToxID)
			emitResult(c.Feature, status, exitCode, details)

		case "shutdown":
			return

		default:
			emitError(fmt.Sprintf("unknown command %q", env.Cmd))
		}
	}

	if err := scanner.Err(); err != nil {
		emitError(fmt.Sprintf("stdin scanner: %v", err))
	}
}
