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

/* ── global node state ────────────────────────────────────────────────────── */
static Tox    *g_tox              = NULL;
static int     g_friend_received  = 0;   /* set by callback */
static uint32_t g_friend_num      = UINT32_MAX;
static int     g_msg_received     = 0;
static char    g_last_msg[2048]   = {0};
static int     g_running          = 1;

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
    (void)role; (void)peer_tox_id;
    /* File transfer testing requires additional callback plumbing.
     * Report not_implemented to keep the test matrix concise in the initial
     * release; a follow-up PR should add full FT support. */
    emit_result("file_transfer", "not_implemented", 2,
                "file transfer test not yet implemented in c-testnode");
}

static void test_conference_invite(const char *role, const char *peer_tox_id)
{
    (void)role; (void)peer_tox_id;
    emit_result("conference_invite", "not_implemented", 2,
                "conference invite test not yet implemented in c-testnode");
}

static void test_conference_message(const char *role, const char *peer_tox_id)
{
    (void)role; (void)peer_tox_id;
    emit_result("conference_message", "not_implemented", 2,
                "conference message test not yet implemented in c-testnode");
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
    /* Initialise Tox. */
    struct Tox_Options *opts = tox_options_new(NULL);
    tox_options_set_ipv6_enabled(opts, false);
    tox_options_set_udp_enabled(opts, true);
    tox_options_set_start_port(opts, 33445);
    tox_options_set_end_port(opts, 33545);

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
