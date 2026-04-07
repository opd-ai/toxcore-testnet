//! rust-testnode — tox-rs/tox implementation test node.
//!
//! Implements the JSON-line IPC protocol used by the toxcore-testnet harness.
//!
//! # Protocol
//!
//! **stdout (node → harness)**
//! ```json
//! {"type":"ready","impl":"tox-rs","tox_id":"HEX","dht_key":"HEX","tox_port":N}
//! {"type":"result","feature":"...","status":"...","exit_code":N,"details":"..."}
//! {"type":"error","message":"..."}
//! ```
//!
//! **stdin (harness → node)**
//! ```json
//! {"cmd":"bootstrap","host":"127.0.0.1","port":N,"key":"HEX"}
//! {"cmd":"run_test","feature":"...","role":"initiator|responder","peer_tox_id":"HEX"}
//! {"cmd":"shutdown"}
//! ```

use std::time::Duration;

use futures::StreamExt;
use hex::{decode as hex_decode, encode_upper as hex_upper};
use serde::{Deserialize, Serialize};
use tokio::io::{AsyncBufReadExt, BufReader};
use tokio::sync::mpsc;
use tokio::time::timeout;

// ── IPC types ──────────────────────────────────────────────────────────────

#[derive(Serialize)]
#[serde(tag = "type", rename_all = "snake_case")]
enum OutMsg {
    Ready {
        #[serde(rename = "impl")]
        impl_name: &'static str,
        tox_id: String,
        dht_key: String,
        tox_port: u16,
    },
    Result {
        feature: String,
        status: String,
        exit_code: i32,
        details: String,
    },
    Error {
        message: String,
    },
}

#[derive(Deserialize, Debug)]
#[serde(tag = "cmd", rename_all = "snake_case")]
enum InCmd {
    Bootstrap {
        host: String,
        port: u16,
        key: String,
    },
    RunTest {
        feature: String,
        role: String,
        peer_tox_id: String,
    },
    Shutdown,
}

fn emit(msg: &OutMsg) {
    println!("{}", serde_json::to_string(msg).unwrap());
}

fn emit_result(feature: impl Into<String>, status: &str, exit_code: i32, details: impl Into<String>) {
    emit(&OutMsg::Result {
        feature: feature.into(),
        status: status.to_string(),
        exit_code,
        details: details.into(),
    });
}

fn emit_error(message: impl Into<String>) {
    emit(&OutMsg::Error { message: message.into() });
}

// ── tox-rs wrappers ────────────────────────────────────────────────────────
//
// The tox-rs/tox API is async and event-driven.  We wrap it in a thin layer
// that exposes synchronous-feeling helpers suitable for the test scenarios.
//
// NOTE: The exact tox-rs public API has evolved across versions.  The code
// below targets the interface available on the `master` branch as of the
// initial testnet implementation.  If the API has changed, adjust the method
// names and types to match.

use tox::toxcore::crypto_core::SecretKey;
use tox::toxcore::dht::packet::DhtPacket;
use tox::toxcore::dht::server::Server as DhtServer;
use tox::toxcore::friend_connection::FriendConnections;
use tox::toxcore::net_crypto::NetCrypto;
use tox::toxcore::onion::client::OnionClient;
use tox::toxcore::tcp::client::Connections as TcpConnections;
use tox::toxcore::tox::Tox;

// ── node state ─────────────────────────────────────────────────────────────

struct Node {
    tox: Tox,
}

impl Node {
    async fn new() -> Result<Self, String> {
        let sk = SecretKey::generate(&mut rand::thread_rng());
        let tox = Tox::new(sk, Default::default())
            .await
            .map_err(|e| format!("Tox::new: {e:?}"))?;
        Ok(Node { tox })
    }

    fn tox_id_hex(&self) -> String {
        hex_upper(self.tox.tox_id().as_bytes())
    }

    fn dht_key_hex(&self) -> String {
        hex_upper(self.tox.dht_pk().as_bytes())
    }

    fn udp_port(&self) -> u16 {
        self.tox.udp_port()
    }

    async fn bootstrap(&mut self, host: &str, port: u16, key_hex: &str) -> Result<(), String> {
        let key_bytes = hex_decode(key_hex)
            .map_err(|e| format!("bad key hex: {e}"))?;
        self.tox
            .bootstrap(host, port, &key_bytes)
            .await
            .map_err(|e| format!("bootstrap: {e:?}"))
    }
}

// ── feature tests ─────────────────────────────────────────────────────────

async fn test_dht_bootstrap(node: &mut Node) -> (String, i32, String) {
    let check = timeout(Duration::from_secs(30), async {
        loop {
            if node.tox.is_connected() {
                return;
            }
            tokio::time::sleep(Duration::from_millis(100)).await;
        }
    })
    .await;

    match check {
        Ok(()) => ("compatible".into(), 0, "DHT peer found".into()),
        Err(_) => ("conflicting".into(), 3, "DHT bootstrap timed out after 30s".into()),
    }
}

async fn test_friend_request(node: &mut Node, role: &str, peer_tox_id: &str)
    -> (String, i32, String)
{
    match role {
        "initiator" => {
            let peer_bytes = match hex_decode(peer_tox_id) {
                Ok(b) => b,
                Err(e) => return ("conflicting".into(), 1, format!("bad peer_tox_id: {e}")),
            };
            if let Err(e) = node.tox.add_friend(&peer_bytes, b"testnet") {
                return ("conflicting".into(), 1, format!("add_friend: {e:?}"));
            }
            // Wait for the friend to come online (up to 60 s).
            let res = timeout(Duration::from_secs(60), async {
                loop {
                    if node.tox.friend_is_connected(0) {
                        return true;
                    }
                    tokio::time::sleep(Duration::from_millis(200)).await;
                }
            })
            .await;
            match res {
                Ok(_) => ("compatible".into(), 0, "friend request accepted and friend online".into()),
                Err(_) => ("conflicting".into(), 3, "friend never came online within 60s".into()),
            }
        }
        "responder" => {
            // Accept incoming friend requests automatically.
            let res = timeout(Duration::from_secs(60), async {
                loop {
                    if let Some(req) = node.tox.next_friend_request().await {
                        // Explicitly accept the request so the peer comes online.
                        if let Err(e) = node.tox.add_friend_norequest(&req.public_key) {
                            return Err(format!("add_friend_norequest: {e:?}"));
                        }
                        return Ok(());
                    }
                    tokio::time::sleep(Duration::from_millis(200)).await;
                }
            })
            .await;
            match res {
                Ok(Ok(())) => ("compatible".into(), 0, "friend request received and accepted".into()),
                Ok(Err(e)) => ("conflicting".into(), 1, e),
                Err(_) => ("conflicting".into(), 3, "no friend request received within 60s".into()),
            }
        }
        _ => ("not_implemented".into(), 2, format!("unknown role: {role}")),
    }
}

async fn test_friend_message(node: &mut Node, role: &str, peer_tox_id: &str)
    -> (String, i32, String)
{
    const PING: &[u8] = b"toxcore-testnet-ping";

    match role {
        "initiator" => {
            let peer_bytes = match hex_decode(peer_tox_id) {
                Ok(b) => b,
                Err(e) => return ("conflicting".into(), 1, format!("bad peer_tox_id: {e}")),
            };
            let _ = node.tox.add_friend(&peer_bytes, b"testnet");
            // Wait for friend online.
            let online = timeout(Duration::from_secs(60), async {
                loop {
                    if node.tox.friend_is_connected(0) { return; }
                    tokio::time::sleep(Duration::from_millis(200)).await;
                }
            }).await;
            if online.is_err() {
                return ("conflicting".into(), 3, "friend never came online".into());
            }
            if let Err(e) = node.tox.send_message(0, PING) {
                return ("conflicting".into(), 1, format!("send_message: {e:?}"));
            }
            // Wait for echo.
            let echo = timeout(Duration::from_secs(30), async {
                loop {
                    if let Some(msg) = node.tox.next_message().await {
                        if msg.content == PING {
                            return;
                        }
                    }
                    tokio::time::sleep(Duration::from_millis(100)).await;
                }
            }).await;
            match echo {
                Ok(()) => ("compatible".into(), 0, "message sent and echo received".into()),
                Err(_) => ("conflicting".into(), 3, "echo not received within 30s".into()),
            }
        }
        "responder" => {
            // Echo every message back for 60 s.
            let _ = timeout(Duration::from_secs(60), async {
                loop {
                    if let Some(msg) = node.tox.next_message().await {
                        let _ = node.tox.send_message(msg.friend_num, &msg.content);
                    }
                    tokio::time::sleep(Duration::from_millis(100)).await;
                }
            }).await;
            ("compatible".into(), 0, "responder ran echo loop".into())
        }
        _ => ("not_implemented".into(), 2, format!("unknown role: {role}")),
    }
}

fn not_impl(feature: &str) -> (String, i32, String) {
    (
        "not_implemented".into(),
        2,
        format!("{feature} test not yet implemented in rust-testnode"),
    )
}

// ── main ───────────────────────────────────────────────────────────────────

#[tokio::main]
async fn main() {
    let mut node = match Node::new().await {
        Ok(n) => n,
        Err(e) => {
            emit_error(format!("failed to initialise Tox node: {e}"));
            std::process::exit(1);
        }
    };

    // Announce readiness.
    emit(&OutMsg::Ready {
        impl_name: "tox-rs",
        tox_id: node.tox_id_hex(),
        dht_key: node.dht_key_hex(),
        tox_port: node.udp_port(),
    });

    // Stdin command loop.
    let stdin = tokio::io::stdin();
    let mut lines = BufReader::new(stdin).lines();

    while let Ok(Some(line)) = lines.next_line().await {
        let line = line.trim().to_string();
        if line.is_empty() {
            continue;
        }

        let cmd: InCmd = match serde_json::from_str(&line) {
            Ok(c) => c,
            Err(e) => {
                emit_error(format!("bad command JSON ({e}): {line}"));
                continue;
            }
        };

        match cmd {
            InCmd::Bootstrap { host, port, key } => {
                if let Err(e) = node.bootstrap(&host, port, &key).await {
                    emit_error(e);
                }
            }

            InCmd::RunTest { feature, role, peer_tox_id } => {
                let (status, exit_code, details) = match feature.as_str() {
                    "dht_bootstrap" => test_dht_bootstrap(&mut node).await,
                    "friend_request" => test_friend_request(&mut node, &role, &peer_tox_id).await,
                    "friend_message" => test_friend_message(&mut node, &role, &peer_tox_id).await,
                    "file_transfer" => not_impl(&feature),
                    "conference_invite" => not_impl(&feature),
                    "conference_message" => not_impl(&feature),
                    other => ("not_implemented".into(), 2, format!("unknown feature: {other}")),
                };
                emit_result(feature, &status, exit_code, details);
            }

            InCmd::Shutdown => break,
        }
    }
}
