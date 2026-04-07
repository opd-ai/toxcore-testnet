//! rust-testnode — tox-rs/tox implementation test node.
//!
//! **STATUS: NOT IMPLEMENTED**
//!
//! The tox-rs/tox library has restructured and no longer provides a high-level
//! `Tox` struct API. The current implementation requires building a full Tox
//! client from low-level primitives (DHT, NetCrypto, FriendConnection, Onion).
//!
//! This test node emits a ready message and then reports all tests as
//! `not_implemented` until a high-level API is available in tox-rs/tox.
//!
//! # Protocol
//!
//! **stdout (node → harness)**
//! ```json
//! {"type":"ready","impl":"tox-rs","tox_id":"STUB","dht_key":"STUB","tox_port":0}
//! {"type":"result","feature":"...","status":"not_implemented","exit_code":2,"details":"..."}
//! {"type":"error","message":"..."}
//! ```
//!
//! **stdin (harness → node)**
//! ```json
//! {"cmd":"bootstrap","host":"127.0.0.1","port":N,"key":"HEX"}
//! {"cmd":"run_test","feature":"...","role":"initiator|responder","peer_tox_id":"HEX"}
//! {"cmd":"shutdown"}
//! ```

use serde::{Deserialize, Serialize};
use tokio::io::{AsyncBufReadExt, BufReader};

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
        #[allow(dead_code)]
        host: String,
        #[allow(dead_code)]
        port: u16,
        #[allow(dead_code)]
        key: String,
    },
    RunTest {
        feature: String,
        #[allow(dead_code)]
        role: String,
        #[allow(dead_code)]
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

const NOT_IMPL_REASON: &str = "tox-rs/tox library restructured; high-level Tox API not available";

// ── main ───────────────────────────────────────────────────────────────────

#[tokio::main]
async fn main() {
    // Emit a stub ready message - we can't actually initialize tox-rs without
    // implementing a full client from low-level primitives.
    emit(&OutMsg::Ready {
        impl_name: "tox-rs",
        tox_id: "0".repeat(76), // Stub Tox ID (76 hex chars)
        dht_key: "0".repeat(64), // Stub DHT key (64 hex chars)
        tox_port: 0,
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
            InCmd::Bootstrap { .. } => {
                // Cannot bootstrap - no Tox instance
                emit_error(NOT_IMPL_REASON);
            }

            InCmd::RunTest { feature, .. } => {
                // All tests report not_implemented
                emit_result(&feature, "not_implemented", 2, NOT_IMPL_REASON);
            }

            InCmd::Shutdown => break,
        }
    }
}
