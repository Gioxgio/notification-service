#define _POSIX_C_SOURCE 200809L

#include "telegram.c"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int passed = 0;
static int failed = 0;

static void sleep_ms(int ms) {
    struct timespec ts = {.tv_sec = ms / 1000,
                          .tv_nsec = (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static void check(bool condition, const char *name) {
    if (condition) {
        printf("PASS: %s\n", name);
        passed++;
    } else {
        printf("FAIL: %s\n", name);
        failed++;
    }
}

// Mock HTTP server

typedef struct {
    int port;
    int status_code;
    const char *response_body;
} mock_server_config_t;

static int mock_server_run(const mock_server_config_t *cfg) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
        return -1;

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)cfg->port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, 1) < 0) {
        close(server_fd);
        return -1;
    }

    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
        close(server_fd);
        return -1;
    }

    /* Read the full HTTP request (simple: read until \r\n\r\n) */
    {
        char buf[8192];
        ssize_t total = 0;
        while (total < (ssize_t)sizeof(buf) - 1) {
            ssize_t n =
                recv(client_fd, buf + total, sizeof(buf) - 1 - total, 0);
            if (n <= 0)
                break;
            total += n;
            buf[total] = '\0';
            if (strstr(buf, "\r\n\r\n"))
                break;
        }
    }

    /**** Send response ****/
    char response[2048];
    int len =
        snprintf(response, sizeof(response),
                 "HTTP/1.1 %d %s\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "\r\n"
                 "%s",
                 cfg->status_code, cfg->status_code == 200 ? "OK" : "Error",
                 strlen(cfg->response_body), cfg->response_body);

    send(client_fd, response, (size_t)len, 0);
    close(client_fd);
    close(server_fd);
    return 0;
}

// Tests for json_escape_string

static void test_json_escape_string(void) {
    char out[256];
    int len;

    /* Plain string — no escaping needed */
    len = json_escape_string("hello", out, sizeof(out));
    check(len == 5 && strcmp(out, "hello") == 0, "escape: plain string");

    /* Double quote */
    len = json_escape_string("say \"hi\"", out, sizeof(out));
    check(len == 10 && strcmp(out, "say \\\"hi\\\"") == 0,
          "escape: double quote");

    /* Backslash */
    len = json_escape_string("a\\b", out, sizeof(out));
    check(len == 4 && strcmp(out, "a\\\\b") == 0, "escape: backslash");

    /* Newline */
    len = json_escape_string("line1\nline2", out, sizeof(out));
    check(len == 12 && strcmp(out, "line1\\nline2") == 0, "escape: newline");

    /* Tab */
    len = json_escape_string("a\tb", out, sizeof(out));
    check(len == 4 && strcmp(out, "a\\tb") == 0, "escape: tab");

    /* Carriage return */
    len = json_escape_string("a\rb", out, sizeof(out));
    check(len == 4 && strcmp(out, "a\\rb") == 0, "escape: carriage return");

    /* Backspace */
    len = json_escape_string("a\bb", out, sizeof(out));
    check(len == 4 && strcmp(out, "a\\bb") == 0, "escape: backspace");

    /* Form feed */
    len = json_escape_string("a\fb", out, sizeof(out));
    check(len == 4 && strcmp(out, "a\\fb") == 0, "escape: form feed");

    /* Control character (0x01) */
    len = json_escape_string("a\x01"
                             "b",
                             out, sizeof(out));
    check(len == 8 && strcmp(out, "a\\u0001b") == 0,
          "escape: control char 0x01");

    /* All standard escapes combined */
    len = json_escape_string("\"\\\n\r\t\b\f", out, sizeof(out));
    check(len == 14 && strcmp(out, "\\\"\\\\\\n\\r\\t\\b\\f") == 0,
          "escape: all standard escapes");

    /* Raw UTF-8 emoji — should pass through unescaped */
    len = json_escape_string("hello\xF0\x9F\x98\x80", out, sizeof(out));
    check(len == 9 && strcmp(out, "hello\xF0\x9F\x98\x80") == 0,
          "escape: raw UTF-8 emoji passthrough");

    /* Raw UTF-8 accented — should pass through */
    len = json_escape_string("caf\xC3\xA9", out, sizeof(out));
    check(len == 5 && strcmp(out, "caf\xC3\xA9") == 0,
          "escape: raw UTF-8 accented passthrough");

    /* Empty string */
    len = json_escape_string("", out, sizeof(out));
    check(len == 0 && strcmp(out, "") == 0, "escape: empty string");

    /* Buffer too small — should return -1 */
    char small[4];
    len = json_escape_string("hello world", small, sizeof(small));
    check(len == -1, "escape: buffer too small returns -1");

    /* Buffer exactly fits "hi" (2 chars + null = 3) */
    char exact[3];
    len = json_escape_string("hi", exact, sizeof(exact));
    check(len == 2 && strcmp(exact, "hi") == 0, "escape: buffer exactly fits");

    /* Buffer one byte too small for "hi" */
    char tight[2];
    len = json_escape_string("hi", tight, sizeof(tight));
    check(len == -1, "escape: buffer one byte too small");

    /* Escaped output needs more room — quote needs 2 chars + null = 3 */
    char small2[2];
    len = json_escape_string("\"", small2, sizeof(small2));
    check(len == -1, "escape: escaped output exceeds buffer");
}

// Tests for write_callback

static void test_write_callback(void) {
    http_response_buf_t buf;
    char buffer[64];
    size_t ret;

    /* Basic write */
    buf.buffer = buffer;
    buf.capacity = sizeof(buffer);
    buf.written = 0;
    ret = write_callback("hello", 1, 5, &buf);
    check(ret == 5, "callback: returns size*nmemb");
    check(buf.written == 5, "callback: written count");
    check(strcmp(buffer, "hello") == 0, "callback: content matches");

    /* Second write appends */
    ret = write_callback(" world", 1, 6, &buf);
    check(ret == 6, "callback: second write returns size*nmemb");
    check(buf.written == 11, "callback: appended written count");
    check(strcmp(buffer, "hello world") == 0, "callback: appended content");

    /* Truncation at capacity */
    buf.buffer = buffer;
    buf.capacity = 8; /* 7 chars + null */
    buf.written = 0;
    ret = write_callback("abcdefghij", 1, 10, &buf);
    check(ret == 10, "callback: truncation still returns full size*nmemb");
    check(buf.written == 7, "callback: truncated written count");
    check(strcmp(buffer, "abcdefg") == 0, "callback: truncated content");
    check(buffer[7] == '\0', "callback: null terminated after truncation");

    /* NULL buffer — should not crash */
    buf.buffer = NULL;
    buf.capacity = 0;
    buf.written = 0;
    ret = write_callback("hello", 1, 5, &buf);
    check(ret == 5, "callback: NULL buffer returns size*nmemb");

    /* Zero capacity — should not crash */
    buf.buffer = buffer;
    buf.capacity = 0;
    buf.written = 0;
    ret = write_callback("hello", 1, 5, &buf);
    check(ret == 5, "callback: zero capacity returns size*nmemb");

    /* Capacity of 1 — only null terminator fits */
    buf.buffer = buffer;
    buf.capacity = 1;
    buf.written = 0;
    ret = write_callback("hello", 1, 5, &buf);
    check(ret == 5, "callback: capacity 1 returns size*nmemb");
    check(buf.written == 0, "callback: capacity 1 written is 0");
    check(buffer[0] == '\0', "callback: capacity 1 null terminated");

    /* Large write that exactly fills capacity */
    buf.buffer = buffer;
    buf.capacity = 11; /* 10 chars + null */
    buf.written = 0;
    ret = write_callback("0123456789", 1, 10, &buf);
    check(ret == 10, "callback: exact fill returns size*nmemb");
    check(buf.written == 10, "callback: exact fill written count");
    check(strcmp(buffer, "0123456789") == 0, "callback: exact fill content");
    check(buffer[10] == '\0', "callback: exact fill null terminated");
}

// Tests for telegram_send_message

static void test_telegram_send_message(void) {
    telegram_config_t config = {.token = "test-token", .chat_id = "12345"};
    notification_payload_t payload = {.message = "hello world"};
    char response_buf[1024];
    int ret;

    /* Start mock server on port 18923 returning 200 */
    mock_server_config_t mock_cfg = {
        .port = 18923, .status_code = 200, .response_body = "{\"ok\":true}"};

    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        mock_server_run(&mock_cfg);
        _exit(0);
    }

    /* parent — give the server a moment to start */
    sleep_ms(100);

    setenv("TELEGRAM_API_URL", "http://127.0.0.1:18923", 1);
    ret = telegram_send_message(&config, &payload, response_buf,
                                sizeof(response_buf));
    check(ret == 200, "send: returns 200 on success");
    check(strstr(response_buf, "{\"ok\":true") != NULL,
          "send: response body captured");

    /* Wait for child */
    int status;
    waitpid(pid, &status, 0);

    /* Test 400 response */
    mock_cfg.status_code = 400;
    mock_cfg.response_body = "{\"ok\":false";

    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        mock_server_run(&mock_cfg);
        _exit(0);
    }

    sleep_ms(100);
    ret = telegram_send_message(&config, &payload, response_buf,
                                sizeof(response_buf));
    check(ret == 400, "send: returns 400 on bad request");

    waitpid(pid, &status, 0);

    /* Test 401 response */
    mock_cfg.status_code = 401;
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        mock_server_run(&mock_cfg);
        _exit(0);
    }

    sleep_ms(100);
    ret = telegram_send_message(&config, &payload, response_buf,
                                sizeof(response_buf));
    check(ret == 401, "send: returns 401 on unauthorized");

    waitpid(pid, &status, 0);

    /* Test 429 response */
    mock_cfg.status_code = 429;
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        mock_server_run(&mock_cfg);
        _exit(0);
    }

    sleep_ms(100);
    ret = telegram_send_message(&config, &payload, response_buf,
                                sizeof(response_buf));
    check(ret == 429, "send: returns 429 on rate limit");

    waitpid(pid, &status, 0);

    /* Test network failure — nothing listening on port 18924 */
    setenv("TELEGRAM_API_URL", "http://127.0.0.1:18924", 1);
    ret = telegram_send_message(&config, &payload, response_buf,
                                sizeof(response_buf));
    check(ret == -1, "send: returns -1 on network failure");

    /* Test with NULL response buffer */
    setenv("TELEGRAM_API_URL", "http://127.0.0.1:18923", 1);
    mock_cfg.status_code = 200;
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        mock_server_run(&mock_cfg);
        _exit(0);
    }

    sleep_ms(100);
    ret = telegram_send_message(&config, &payload, NULL, 0);
    check(ret == 200, "send: NULL response buffer still returns 200");

    waitpid(pid, &status, 0);

    /* Test with special characters in message */
    strcpy(payload.message, "hello \"world\"\nnew line\ttab");
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        mock_server_run(&mock_cfg);
        _exit(0);
    }

    sleep_ms(100);
    ret = telegram_send_message(&config, &payload, response_buf,
                                sizeof(response_buf));
    check(ret == 200, "send: special characters in message");

    waitpid(pid, &status, 0);

    /* Restore */
    unsetenv("TELEGRAM_API_URL");
}

// Main

int main(void) {
    printf("=== json_escape_string tests ===\n");
    test_json_escape_string();

    printf("\n=== write_callback tests ===\n");
    test_write_callback();

    printf("\n=== telegram_send_message tests ===\n");
    test_telegram_send_message();

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
