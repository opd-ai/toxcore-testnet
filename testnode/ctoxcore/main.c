/*
 * c-testnode — TokTok/c-toxcore implementation test node.
 *
 * Implements the JSON-line IPC protocol expected by the toxcore-testnet harness:
 *
 *   stdout (node → harness):
 *     {"type":"ready","impl":"c-toxcore","tox_id":"HEX","dht_key":"HEX","tox_port":N}
 *     {"type":"result","feature":"...","status":"...","exit_code":N,"details":"..."}
 *     {"type":"error","message":"..."}
 *
 *   stdin (harness → node):
 *     {"cmd":"bootstrap","host":"127.0.0.1","port":N,"key":"HEX"}
 *     {"cmd":"run_test","feature":"...","role":"initiator|responder","peer_tox_id":"HEX"}
 *     {"cmd":"shutdown"}
 *
 * Build: see CMakeLists.txt
 */

#include <tox/tox.h>

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/select.h>
#endif

/* ── constants ────────────────────────────────────────────────────────────── */
#define HEX_ADDR_LEN   (TOX_ADDRESS_SIZE * 2 + 1)
#define HEX_PUBKEY_LEN (TOX_PUBLIC_KEY_SIZE * 2 + 1)
#define CMD_BUF        4096
#define FILE_XFER_SIZE 1024  /* Size of test file blob */

/* ── global node state ────────────────────────────────────────────────────── */
static Tox    *g_tox              = NULL;
static int     g_friend_received  = 0;   /* set by callback */
static uint32_t g_friend_num      = UINT32_MAX;
static int     g_msg_received     = 0;
static char    g_last_msg[2048]   = {0};
static int     g_running          = 1;

/* File transfer state */
static uint8_t  g_file_data[FILE_XFER_SIZE];      /* buffer for received file */
static size_t   g_file_pos         = 0;           /* current receive position */
static int      g_file_complete    = 0;           /* set when transfer completes */
static uint32_t g_file_num         = UINT32_MAX;  /* file number for sending */
static size_t   g_file_send_pos    = 0;           /* send position */
static int      g_file_send_done   = 0;           /* sending complete flag */

/* Conference state */
static uint32_t g_conference_num   = UINT32_MAX;  /* conference number */
static int      g_conference_joined = 0;          /* set when joined conference */
static int      g_conference_msg_received = 0;    /* set when conference message received */
static uint8_t  g_conference_cookie[TOX_CONFERENCE_UID_SIZE];  /* invite cookie */
static size_t   g_conference_cookie_len = 0;

/* ── hex helpers ──────────────────────────────────────────────────────────── */
static void bytes_to_hex(const uint8_t *in, size_t len, char *out)
{
    static const char t[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++) {
        out[2*i]   = t[in[i] >> 4];
        out[2*i+1] = t[in[i] & 0x0f];
    }
    out[2*len] = '\0';
}

static int hex_char(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_to_bytes(const char *hex, uint8_t *out, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        int hi = hex_char(hex[2*i]);
        int lo = hex_char(hex[2*i+1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* ── minimal JSON field extraction ───────────────────────────────────────── */
/*
 * Extracts the value of a string-type JSON field.  Returns 1 on success.
 * This is intentionally minimal — it only handles well-formed single-line JSON
 * produced by the Go harness.
 */
static int json_str(const char *json, const char *key, char *val, size_t vlen)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < vlen) val[i++] = *p++;
    val[i] = '\0';
    return 1;
}

static int json_int(const char *json, const char *key, int *val)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (!isdigit((unsigned char)*p)) return 0;
    *val = atoi(p);
    return 1;
}

/* ── JSON output helpers ──────────────────────────────────────────────────── */

/* Write a JSON-safe string value (without surrounding quotes) to stdout. */
static void json_write_escaped(FILE *out, const char *s)
{
    if (s == NULL) return;
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; ++p) {
        switch (*p) {
            case '"':  fputs("\\\"", out); break;
            case '\\': fputs("\\\\", out); break;
            case '\b': fputs("\\b",  out); break;
            case '\f': fputs("\\f",  out); break;
            case '\n': fputs("\\n",  out); break;
            case '\r': fputs("\\r",  out); break;
            case '\t': fputs("\\t",  out); break;
            default:
                if (*p < 0x20)
                    fprintf(out, "\\u%04x", (unsigned)*p);
                else
                    fputc(*p, out);
                break;
        }
    }
}

static void emit_ready(const char *tox_id, const char *dht_key, uint16_t port)
{
    fputs("{\"type\":\"ready\",\"impl\":\"c-toxcore\",\"tox_id\":\"", stdout);
    json_write_escaped(stdout, tox_id);
    fputs("\",\"dht_key\":\"", stdout);
    json_write_escaped(stdout, dht_key);
    fprintf(stdout, "\",\"tox_port\":%u}\n", (unsigned)port);
    fflush(stdout);
}

static void emit_result(const char *feature, const char *status,
                        int exit_code, const char *details)
{
    fputs("{\"type\":\"result\",\"feature\":\"", stdout);
    json_write_escaped(stdout, feature);
    fputs("\",\"status\":\"", stdout);
    json_write_escaped(stdout, status);
    fprintf(stdout, "\",\"exit_code\":%d,\"details\":\"", exit_code);
    json_write_escaped(stdout, details);
    fputs("\"}\n", stdout);
    fflush(stdout);
}

static void emit_error(const char *msg)
{
    fputs("{\"type\":\"error\",\"message\":\"", stdout);
    json_write_escaped(stdout, msg);
    fputs("\"}\n", stdout);
    fflush(stdout);
}

/* ── Tox callbacks ────────────────────────────────────────────────────────── */
static void cb_friend_request(Tox *tox,
                               const uint8_t *pub_key,
                               const uint8_t *message, size_t length,
                               void *user_data)
{
    (void)message; (void)length; (void)user_data;
    TOX_ERR_FRIEND_ADD err;
    g_friend_num = tox_friend_add_norequest(tox, pub_key, &err);
    if (err == TOX_ERR_FRIEND_ADD_OK || err == TOX_ERR_FRIEND_ADD_ALREADY_SENT) {
        g_friend_received = 1;
    }
}

static void cb_friend_message(Tox *tox, uint32_t friend_num,
                               TOX_MESSAGE_TYPE type,
                               const uint8_t *message, size_t length,
                               void *user_data)
{
    (void)user_data;
    size_t copy_len = length < sizeof(g_last_msg) - 1
                      ? length : sizeof(g_last_msg) - 1;
    memcpy(g_last_msg, message, copy_len);
    g_last_msg[copy_len] = '\0';
    g_msg_received = 1;

    /* Echo the message back (responder role). */
    TOX_ERR_FRIEND_SEND_MESSAGE err;
    tox_friend_send_message(tox, friend_num, type, message, length, &err);
}

/* ── file transfer callbacks ──────────────────────────────────────────────── */
static void cb_file_recv(Tox *tox, uint32_t friend_num, uint32_t file_num,
                         uint32_t kind, uint64_t file_size,
                         const uint8_t *filename, size_t filename_len,
                         void *user_data)
{
    (void)user_data; (void)filename; (void)filename_len;
    if (kind != TOX_FILE_KIND_DATA || file_size > FILE_XFER_SIZE) {
        tox_file_control(tox, friend_num, file_num, TOX_FILE_CONTROL_CANCEL, NULL);
        return;
    }
    /* Accept the file transfer */
    TOX_ERR_FILE_CONTROL ctrl_err;
    tox_file_control(tox, friend_num, file_num, TOX_FILE_CONTROL_RESUME, &ctrl_err);
    g_file_pos = 0;
    g_file_complete = 0;
}

static void cb_file_recv_chunk(Tox *tox, uint32_t friend_num, uint32_t file_num,
                                uint64_t position, const uint8_t *data, size_t length,
                                void *user_data)
{
    (void)tox; (void)friend_num; (void)file_num; (void)user_data;
    if (length == 0) {
        /* Transfer complete */
        g_file_complete = 1;
        return;
    }
    if (position + length <= FILE_XFER_SIZE) {
        memcpy(g_file_data + position, data, length);
        g_file_pos = position + length;
    }
}

static void cb_file_chunk_request(Tox *tox, uint32_t friend_num, uint32_t file_num,
                                   uint64_t position, size_t length,
                                   void *user_data)
{
    (void)user_data;
    if (length == 0) {
        /* Transfer complete from our side */
        g_file_send_done = 1;
        return;
    }
    /* Generate deterministic test data: bytes are position mod 256 */
    uint8_t chunk[length];
    for (size_t i = 0; i < length; i++) {
        chunk[i] = (uint8_t)((position + i) % 256);
    }
    TOX_ERR_FILE_SEND_CHUNK err;
    tox_file_send_chunk(tox, friend_num, file_num, position, chunk, length, &err);
}

/* ── conference callbacks ─────────────────────────────────────────────────── */
static void cb_conference_invite(Tox *tox, uint32_t friend_num, 
                                  TOX_CONFERENCE_TYPE type,
                                  const uint8_t *cookie, size_t length,
                                  void *user_data)
{
    (void)tox; (void)user_data;
    if (type != TOX_CONFERENCE_TYPE_TEXT) {
        return;
    }
    /* Store the cookie to join later */
    if (length <= sizeof(g_conference_cookie)) {
        memcpy(g_conference_cookie, cookie, length);
        g_conference_cookie_len = length;
    }
}

static void cb_conference_connected(Tox *tox, uint32_t conference_num, void *user_data)
{
    (void)tox; (void)user_data;
    g_conference_joined = 1;
    g_conference_num = conference_num;
}

static void cb_conference_message(Tox *tox, uint32_t conference_num,
                                   uint32_t peer_num, TOX_MESSAGE_TYPE type,
                                   const uint8_t *message, size_t length,
                                   void *user_data)
{
    (void)tox; (void)conference_num; (void)peer_num; (void)type;
    (void)message; (void)length; (void)user_data;
    g_conference_msg_received = 1;
}

/* ── iterate helper ───────────────────────────────────────────────────────── */
static void tox_iterate_for(uint32_t ms)
{
    uint32_t elapsed = 0;
    while (elapsed < ms) {
        tox_iterate(g_tox, NULL);
        uint32_t interval = tox_iteration_interval(g_tox);
        usleep(interval * 1000u);
        elapsed += interval;
    }
}

/* ── feature tests ────────────────────────────────────────────────────────── */
static void test_dht_bootstrap(const char *role, const char *peer_tox_id)
{
    (void)role; (void)peer_tox_id;
    /* Give the DHT 30 s to find at least one peer. */
    for (int i = 0; i < 300; i++) {
        tox_iterate(g_tox, NULL);
        uint32_t interval = tox_iteration_interval(g_tox);
        usleep(interval * 1000u);
        if (tox_self_get_connection_status(g_tox) != TOX_CONNECTION_NONE) {
            emit_result("dht_bootstrap", "compatible", 0, "DHT peer found");
            return;
        }
    }
    emit_result("dht_bootstrap", "conflicting", 3,
                "DHT bootstrap timed out after 30s");
}

static void test_friend_request(const char *role, const char *peer_tox_id)
{
    if (strcmp(role, "initiator") == 0) {
        uint8_t addr[TOX_ADDRESS_SIZE];
        if (hex_to_bytes(peer_tox_id, addr, TOX_ADDRESS_SIZE) != 0) {
            emit_result("friend_request", "conflicting", 1,
                        "bad peer_tox_id hex");
            return;
        }
        TOX_ERR_FRIEND_ADD err;
        tox_friend_add(g_tox, addr, (const uint8_t *)"testnet", 7, &err);
        if (err != TOX_ERR_FRIEND_ADD_OK && err != TOX_ERR_FRIEND_ADD_ALREADY_SENT) {
            char details[64];
            snprintf(details, sizeof(details),
                     "tox_friend_add failed: %d", (int)err);
            emit_result("friend_request", "conflicting", 1, details);
            return;
        }
        /* Wait up to 60 s for friend to come online. */
        for (int i = 0; i < 600; i++) {
            tox_iterate(g_tox, NULL);
            uint32_t interval = tox_iteration_interval(g_tox);
            usleep(interval * 1000u);
            if (tox_friend_get_connection_status(g_tox, 0, NULL)
                    != TOX_CONNECTION_NONE) {
                emit_result("friend_request", "compatible", 0,
                            "friend request accepted and friend online");
                return;
            }
        }
        emit_result("friend_request", "conflicting", 3,
                    "friend never came online within 60s");
    } else {
        /* Responder: wait for a friend request via callback. */
        for (int i = 0; i < 600; i++) {
            tox_iterate(g_tox, NULL);
            uint32_t interval = tox_iteration_interval(g_tox);
            usleep(interval * 1000u);
            if (g_friend_received) {
                emit_result("friend_request", "compatible", 0,
                            "friend request received and accepted");
                return;
            }
        }
        emit_result("friend_request", "conflicting", 3,
                    "no friend request received within 60s");
    }
}

static void test_friend_message(const char *role, const char *peer_tox_id)
{
    if (strcmp(role, "initiator") == 0) {
        uint8_t addr[TOX_ADDRESS_SIZE];
        if (hex_to_bytes(peer_tox_id, addr, TOX_ADDRESS_SIZE) != 0) {
            emit_result("friend_message", "conflicting", 1,
                        "bad peer_tox_id hex");
            return;
        }
        TOX_ERR_FRIEND_ADD aerr;
        uint32_t friend_num = tox_friend_add(
            g_tox, addr, (const uint8_t *)"testnet", 7, &aerr);
        if (aerr != TOX_ERR_FRIEND_ADD_OK
            && aerr != TOX_ERR_FRIEND_ADD_ALREADY_SENT) {
            emit_result("friend_message", "conflicting", 1,
                        "tox_friend_add failed");
            return;
        }
        /* Wait for friend online. */
        int online = 0;
        for (int i = 0; i < 600 && !online; i++) {
            tox_iterate(g_tox, NULL);
            usleep(tox_iteration_interval(g_tox) * 1000u);
            online = tox_friend_get_connection_status(g_tox, friend_num, NULL)
                     != TOX_CONNECTION_NONE;
        }
        if (!online) {
            emit_result("friend_message", "conflicting", 3,
                        "friend never came online");
            return;
        }
        static const uint8_t msg[] = "toxcore-testnet-ping";
        TOX_ERR_FRIEND_SEND_MESSAGE serr;
        tox_friend_send_message(g_tox, friend_num, TOX_MESSAGE_TYPE_NORMAL,
                                msg, sizeof(msg)-1, &serr);
        if (serr != TOX_ERR_FRIEND_SEND_MESSAGE_OK) {
            emit_result("friend_message", "conflicting", 1,
                        "tox_friend_send_message failed");
            return;
        }
        /* Wait for echo. */
        g_msg_received = 0;
        for (int i = 0; i < 300; i++) {
            tox_iterate(g_tox, NULL);
            usleep(tox_iteration_interval(g_tox) * 1000u);
            if (g_msg_received
                && strcmp(g_last_msg, "toxcore-testnet-ping") == 0) {
                emit_result("friend_message", "compatible", 0,
                            "message sent and echo received");
                return;
            }
        }
        emit_result("friend_message", "conflicting", 3,
                    "echo not received within 30s");
    } else {
        /* Responder: echo loop handled entirely by cb_friend_message callback. */
        tox_iterate_for(60000);
        emit_result("friend_message", "compatible", 0,
                    "responder ran echo loop");
    }
}

static void test_file_transfer(const char *role, const char *peer_tox_id)
{
    if (strcmp(role, "initiator") == 0) {
        /* Initiator: ensure we're friends first, then send a file */
        uint8_t addr[TOX_ADDRESS_SIZE];
        if (hex_to_bytes(peer_tox_id, addr, TOX_ADDRESS_SIZE) != 0) {
            emit_result("file_transfer", "conflicting", 1, "bad peer_tox_id hex");
            return;
        }
        
        /* Add friend if not already */
        TOX_ERR_FRIEND_ADD add_err;
        uint32_t friend_num = tox_friend_add(g_tox, addr, 
                                              (const uint8_t *)"file-test", 9, &add_err);
        if (add_err != TOX_ERR_FRIEND_ADD_OK && add_err != TOX_ERR_FRIEND_ADD_ALREADY_SENT) {
            friend_num = tox_friend_by_public_key(g_tox, addr, NULL);
        }
        
        /* Wait for friend to come online (up to 60s) */
        for (int i = 0; i < 600; i++) {
            tox_iterate(g_tox, NULL);
            usleep(tox_iteration_interval(g_tox) * 1000u);
            if (tox_friend_get_connection_status(g_tox, friend_num, NULL) 
                != TOX_CONNECTION_NONE) {
                break;
            }
        }
        if (tox_friend_get_connection_status(g_tox, friend_num, NULL) 
            == TOX_CONNECTION_NONE) {
            emit_result("file_transfer", "conflicting", 3,
                        "friend never came online for file transfer");
            return;
        }
        
        /* Send file: 1KB of deterministic data */
        TOX_ERR_FILE_SEND send_err;
        g_file_num = tox_file_send(g_tox, friend_num, TOX_FILE_KIND_DATA,
                                    FILE_XFER_SIZE, NULL, 
                                    (const uint8_t *)"testblob", 8, &send_err);
        if (send_err != TOX_ERR_FILE_SEND_OK) {
            char details[64];
            snprintf(details, sizeof(details), "tox_file_send failed: %d", (int)send_err);
            emit_result("file_transfer", "conflicting", 1, details);
            return;
        }
        
        g_file_send_pos = 0;
        g_file_send_done = 0;
        
        /* Wait for transfer to complete (up to 30s) */
        for (int i = 0; i < 300 && !g_file_send_done; i++) {
            tox_iterate(g_tox, NULL);
            usleep(tox_iteration_interval(g_tox) * 1000u);
        }
        
        if (g_file_send_done) {
            emit_result("file_transfer", "compatible", 0, 
                        "file sent successfully");
        } else {
            emit_result("file_transfer", "conflicting", 3,
                        "file send timed out after 30s");
        }
    } else {
        /* Responder: accept file and verify integrity */
        g_file_pos = 0;
        g_file_complete = 0;
        memset(g_file_data, 0, sizeof(g_file_data));
        
        /* Wait for file transfer to complete (up to 60s) */
        for (int i = 0; i < 600 && !g_file_complete; i++) {
            tox_iterate(g_tox, NULL);
            usleep(tox_iteration_interval(g_tox) * 1000u);
        }
        
        if (!g_file_complete) {
            emit_result("file_transfer", "conflicting", 3,
                        "file receive timed out after 60s");
            return;
        }
        
        /* Verify data integrity: each byte should be position mod 256 */
        int valid = 1;
        for (size_t i = 0; i < g_file_pos && valid; i++) {
            if (g_file_data[i] != (uint8_t)(i % 256)) {
                valid = 0;
            }
        }
        
        if (valid && g_file_pos == FILE_XFER_SIZE) {
            emit_result("file_transfer", "compatible", 0,
                        "file received and verified");
        } else {
            char details[64];
            snprintf(details, sizeof(details), 
                     "file data mismatch: got %zu bytes", g_file_pos);
            emit_result("file_transfer", "conflicting", 1, details);
        }
    }
}

static void test_conference_invite(const char *role, const char *peer_tox_id)
{
    if (strcmp(role, "initiator") == 0) {
        /* Initiator: create conference and invite peer */
        uint8_t addr[TOX_ADDRESS_SIZE];
        if (hex_to_bytes(peer_tox_id, addr, TOX_ADDRESS_SIZE) != 0) {
            emit_result("conference_invite", "conflicting", 1, "bad peer_tox_id hex");
            return;
        }
        
        /* Add friend if not already */
        TOX_ERR_FRIEND_ADD add_err;
        uint32_t friend_num = tox_friend_add(g_tox, addr, 
                                              (const uint8_t *)"conf-test", 9, &add_err);
        if (add_err != TOX_ERR_FRIEND_ADD_OK && add_err != TOX_ERR_FRIEND_ADD_ALREADY_SENT) {
            friend_num = tox_friend_by_public_key(g_tox, addr, NULL);
        }
        
        /* Wait for friend to come online (up to 60s) */
        for (int i = 0; i < 600; i++) {
            tox_iterate(g_tox, NULL);
            usleep(tox_iteration_interval(g_tox) * 1000u);
            if (tox_friend_get_connection_status(g_tox, friend_num, NULL) 
                != TOX_CONNECTION_NONE) {
                break;
            }
        }
        if (tox_friend_get_connection_status(g_tox, friend_num, NULL) 
            == TOX_CONNECTION_NONE) {
            emit_result("conference_invite", "conflicting", 3,
                        "friend never came online for conference");
            return;
        }
        
        /* Create conference */
        TOX_ERR_CONFERENCE_NEW new_err;
        g_conference_num = tox_conference_new(g_tox, &new_err);
        if (new_err != TOX_ERR_CONFERENCE_NEW_OK) {
            char details[64];
            snprintf(details, sizeof(details), "tox_conference_new failed: %d", (int)new_err);
            emit_result("conference_invite", "conflicting", 1, details);
            return;
        }
        
        /* Invite friend */
        TOX_ERR_CONFERENCE_INVITE inv_err;
        tox_conference_invite(g_tox, friend_num, g_conference_num, &inv_err);
        if (inv_err != TOX_ERR_CONFERENCE_INVITE_OK) {
            char details[64];
            snprintf(details, sizeof(details), "tox_conference_invite failed: %d", (int)inv_err);
            emit_result("conference_invite", "conflicting", 1, details);
            return;
        }
        
        /* Wait for peer count to be at least 2 (us + peer), up to 30s */
        for (int i = 0; i < 300; i++) {
            tox_iterate(g_tox, NULL);
            usleep(tox_iteration_interval(g_tox) * 1000u);
            TOX_ERR_CONFERENCE_PEER_QUERY peer_err;
            uint32_t peer_count = tox_conference_peer_count(g_tox, g_conference_num, &peer_err);
            if (peer_err == TOX_ERR_CONFERENCE_PEER_QUERY_OK && peer_count >= 2) {
                emit_result("conference_invite", "compatible", 0,
                            "conference created and peer joined");
                return;
            }
        }
        emit_result("conference_invite", "conflicting", 3,
                    "peer did not join conference within 30s");
    } else {
        /* Responder: wait for invite and join */
        g_conference_cookie_len = 0;
        g_conference_joined = 0;
        
        /* Wait for invite (up to 60s) */
        for (int i = 0; i < 600 && g_conference_cookie_len == 0; i++) {
            tox_iterate(g_tox, NULL);
            usleep(tox_iteration_interval(g_tox) * 1000u);
        }
        
        if (g_conference_cookie_len == 0) {
            emit_result("conference_invite", "conflicting", 3,
                        "no conference invite received within 60s");
            return;
        }
        
        /* Join the conference */
        TOX_ERR_CONFERENCE_JOIN join_err;
        g_conference_num = tox_conference_join(g_tox, g_friend_num, 
                                                g_conference_cookie, g_conference_cookie_len,
                                                &join_err);
        if (join_err != TOX_ERR_CONFERENCE_JOIN_OK) {
            char details[64];
            snprintf(details, sizeof(details), "tox_conference_join failed: %d", (int)join_err);
            emit_result("conference_invite", "conflicting", 1, details);
            return;
        }
        
        /* Wait for connected callback (up to 30s) */
        for (int i = 0; i < 300 && !g_conference_joined; i++) {
            tox_iterate(g_tox, NULL);
            usleep(tox_iteration_interval(g_tox) * 1000u);
        }
        
        if (g_conference_joined) {
            emit_result("conference_invite", "compatible", 0,
                        "received invite and joined conference");
        } else {
            emit_result("conference_invite", "conflicting", 3,
                        "conference join did not complete within 30s");
        }
    }
}

static void test_conference_message(const char *role, const char *peer_tox_id)
{
    /* First, both sides need to establish a conference (reuse conference_invite logic) */
    if (strcmp(role, "initiator") == 0) {
        /* Create conference, invite peer, wait for join, then send message */
        uint8_t addr[TOX_ADDRESS_SIZE];
        if (hex_to_bytes(peer_tox_id, addr, TOX_ADDRESS_SIZE) != 0) {
            emit_result("conference_message", "conflicting", 1, "bad peer_tox_id hex");
            return;
        }
        
        /* Add friend if not already */
        TOX_ERR_FRIEND_ADD add_err;
        uint32_t friend_num = tox_friend_add(g_tox, addr, 
                                              (const uint8_t *)"conf-msg", 8, &add_err);
        if (add_err != TOX_ERR_FRIEND_ADD_OK && add_err != TOX_ERR_FRIEND_ADD_ALREADY_SENT) {
            friend_num = tox_friend_by_public_key(g_tox, addr, NULL);
        }
        
        /* Wait for friend online (up to 60s) */
        for (int i = 0; i < 600; i++) {
            tox_iterate(g_tox, NULL);
            usleep(tox_iteration_interval(g_tox) * 1000u);
            if (tox_friend_get_connection_status(g_tox, friend_num, NULL) 
                != TOX_CONNECTION_NONE) {
                break;
            }
        }
        if (tox_friend_get_connection_status(g_tox, friend_num, NULL) 
            == TOX_CONNECTION_NONE) {
            emit_result("conference_message", "conflicting", 3,
                        "friend never came online");
            return;
        }
        
        /* Create and setup conference */
        TOX_ERR_CONFERENCE_NEW new_err;
        g_conference_num = tox_conference_new(g_tox, &new_err);
        if (new_err != TOX_ERR_CONFERENCE_NEW_OK) {
            emit_result("conference_message", "conflicting", 1, "conference creation failed");
            return;
        }
        
        TOX_ERR_CONFERENCE_INVITE inv_err;
        tox_conference_invite(g_tox, friend_num, g_conference_num, &inv_err);
        
        /* Wait for peer to join (up to 30s) */
        for (int i = 0; i < 300; i++) {
            tox_iterate(g_tox, NULL);
            usleep(tox_iteration_interval(g_tox) * 1000u);
            TOX_ERR_CONFERENCE_PEER_QUERY peer_err;
            uint32_t peer_count = tox_conference_peer_count(g_tox, g_conference_num, &peer_err);
            if (peer_err == TOX_ERR_CONFERENCE_PEER_QUERY_OK && peer_count >= 2) {
                break;
            }
        }
        
        /* Send test message */
        const char *test_msg = "testnet-conference-ping";
        TOX_ERR_CONFERENCE_SEND_MESSAGE msg_err;
        tox_conference_send_message(g_tox, g_conference_num, TOX_MESSAGE_TYPE_NORMAL,
                                     (const uint8_t *)test_msg, strlen(test_msg), &msg_err);
        if (msg_err != TOX_ERR_CONFERENCE_SEND_MESSAGE_OK) {
            char details[64];
            snprintf(details, sizeof(details), "conference_send_message failed: %d", (int)msg_err);
            emit_result("conference_message", "conflicting", 1, details);
            return;
        }
        
        emit_result("conference_message", "compatible", 0,
                    "conference message sent");
    } else {
        /* Responder: join conference, wait for message */
        g_conference_cookie_len = 0;
        g_conference_joined = 0;
        g_conference_msg_received = 0;
        
        /* Wait for invite (up to 60s) */
        for (int i = 0; i < 600 && g_conference_cookie_len == 0; i++) {
            tox_iterate(g_tox, NULL);
            usleep(tox_iteration_interval(g_tox) * 1000u);
        }
        
        if (g_conference_cookie_len == 0) {
            emit_result("conference_message", "conflicting", 3,
                        "no conference invite received");
            return;
        }
        
        /* Join */
        TOX_ERR_CONFERENCE_JOIN join_err;
        g_conference_num = tox_conference_join(g_tox, g_friend_num,
                                                g_conference_cookie, g_conference_cookie_len,
                                                &join_err);
        
        /* Wait for message (up to 60s) */
        for (int i = 0; i < 600 && !g_conference_msg_received; i++) {
            tox_iterate(g_tox, NULL);
            usleep(tox_iteration_interval(g_tox) * 1000u);
        }
        
        if (g_conference_msg_received) {
            emit_result("conference_message", "compatible", 0,
                        "conference message received");
        } else {
            emit_result("conference_message", "conflicting", 3,
                        "no conference message received within 60s");
        }
    }
}

/* ── command dispatcher ───────────────────────────────────────────────────── */
static void dispatch(const char *line)
{
    char cmd[64] = {0};
    if (!json_str(line, "cmd", cmd, sizeof(cmd))) {
        emit_error("missing cmd field");
        return;
    }

    if (strcmp(cmd, "bootstrap") == 0) {
        char host[256] = {0}, key_hex[HEX_PUBKEY_LEN] = {0};
        int  port = 0;
        json_str(line, "host", host, sizeof(host));
        json_str(line, "key",  key_hex, sizeof(key_hex));
        json_int(line, "port", &port);

        uint8_t key[TOX_PUBLIC_KEY_SIZE];
        if (hex_to_bytes(key_hex, key, TOX_PUBLIC_KEY_SIZE) != 0) {
            emit_error("bad bootstrap key hex");
            return;
        }
        TOX_ERR_BOOTSTRAP err;
        tox_bootstrap(g_tox, host, (uint16_t)port, key, &err);
        /* No explicit ack; harness does not wait for one. */

    } else if (strcmp(cmd, "run_test") == 0) {
        char feature[64] = {0}, role[32] = {0}, peer[HEX_ADDR_LEN] = {0};
        json_str(line, "feature",    feature, sizeof(feature));
        json_str(line, "role",       role,    sizeof(role));
        json_str(line, "peer_tox_id", peer,   sizeof(peer));

        if      (strcmp(feature, "dht_bootstrap")    == 0) test_dht_bootstrap(role, peer);
        else if (strcmp(feature, "friend_request")   == 0) test_friend_request(role, peer);
        else if (strcmp(feature, "friend_message")   == 0) test_friend_message(role, peer);
        else if (strcmp(feature, "file_transfer")    == 0) test_file_transfer(role, peer);
        else if (strcmp(feature, "conference_invite") == 0) test_conference_invite(role, peer);
        else if (strcmp(feature, "conference_message") == 0) test_conference_message(role, peer);
        else {
            char msg[128];
            snprintf(msg, sizeof(msg), "unknown feature: %s", feature);
            emit_result(feature, "not_implemented", 2, msg);
        }

    } else if (strcmp(cmd, "shutdown") == 0) {
        g_running = 0;

    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "unknown command: %s", cmd);
        emit_error(msg);
    }
}

/* ── main ─────────────────────────────────────────────────────────────────── */
int main(void)
{
    /* Read port range from environment (set by the integration test harness).
     * Each node gets a unique, non-overlapping 100-port window so parallel
     * test instances don't contend for the same ports. */
    int start_port = 33445;
    int end_port   = 33545;
    const char *sp = getenv("TOX_PORT_START");
    const char *ep = getenv("TOX_PORT_END");
    if (sp) { start_port = atoi(sp); }
    if (ep) { end_port   = atoi(ep); }

    /* Initialise Tox. */
    struct Tox_Options *opts = tox_options_new(NULL);
    tox_options_set_ipv6_enabled(opts, false);
    tox_options_set_udp_enabled(opts, true);
    tox_options_set_start_port(opts, (uint16_t)start_port);
    tox_options_set_end_port(opts, (uint16_t)end_port);

    TOX_ERR_NEW err;
    g_tox = tox_new(opts, &err);
    tox_options_free(opts);

    if (!g_tox) {
        char msg[64];
        snprintf(msg, sizeof(msg), "tox_new failed: %d", (int)err);
        emit_error(msg);
        return 1;
    }

    /* Register callbacks. */
    tox_callback_friend_request(g_tox, cb_friend_request);
    tox_callback_friend_message(g_tox, cb_friend_message);
    tox_callback_file_recv(g_tox, cb_file_recv);
    tox_callback_file_recv_chunk(g_tox, cb_file_recv_chunk);
    tox_callback_file_chunk_request(g_tox, cb_file_chunk_request);
    tox_callback_conference_invite(g_tox, cb_conference_invite);
    tox_callback_conference_connected(g_tox, cb_conference_connected);
    tox_callback_conference_message(g_tox, cb_conference_message);

    /* Announce readiness. */
    uint8_t addr[TOX_ADDRESS_SIZE];
    tox_self_get_address(g_tox, addr);
    char addr_hex[HEX_ADDR_LEN];
    bytes_to_hex(addr, TOX_ADDRESS_SIZE, addr_hex);

    uint8_t dht_key[TOX_PUBLIC_KEY_SIZE];
    tox_self_get_dht_id(g_tox, dht_key);
    char dht_hex[HEX_PUBKEY_LEN];
    bytes_to_hex(dht_key, TOX_PUBLIC_KEY_SIZE, dht_hex);

    TOX_ERR_GET_PORT perr;
    uint16_t udp_port = tox_self_get_udp_port(g_tox, &perr);

    emit_ready(addr_hex, dht_hex, udp_port);

    /* Command loop: read JSON lines from stdin; run tox_iterate in between. */
    char line[CMD_BUF];
    struct timeval tv;
    fd_set fds;

    while (g_running) {
        /* Run one Tox iteration. */
        tox_iterate(g_tox, NULL);
        uint32_t interval_ms = tox_iteration_interval(g_tox);

        /* Check stdin without blocking longer than one Tox interval. */
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        tv.tv_sec  = 0;
        tv.tv_usec = (long)interval_ms * 1000L;

        int ready = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
        if (ready > 0) {
            if (!fgets(line, sizeof(line), stdin)) break;
            /* Remove trailing newline. */
            line[strcspn(line, "\r\n")] = '\0';
            if (strlen(line) > 0) dispatch(line);
        } else if (ready < 0 && errno != EINTR) {
            break;
        }
    }

    tox_kill(g_tox);
    return 0;
}
