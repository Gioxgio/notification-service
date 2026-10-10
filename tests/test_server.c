#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "server.c"

#include <arpa/inet.h>
#include <curl/curl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SERVER_PORT 18931
#define MOCK_TELEGRAM_PORT 18932
#define DEAD_TELEGRAM_PORT 18933

static int passed = 0;
static int failed = 0;
static int errors = 0;

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
        errors++;
    }
}

/* ---------- tests for find_header_end ---------- */

static void test_find_header_end(void) {
    char buf[64];

    /* "POST / HTTP/1.1" then the terminator: verify the exact offset. */
    strcpy(buf, "POST / HTTP/1.1\r\n\r\nBODY");
    const char *expected = strstr(buf, "\r\n\r\n");
    char *end = find_header_end(buf, 24);
    check(end != NULL && expected != NULL && end == expected,
          "header_end: finds terminator");

    /* A terminator split across two reads is only found once both halves land.
     */
    strcpy(buf, "X: 1\r\n\r\n");
    check(find_header_end(buf, 6) == NULL,
          "header_end: partial terminator not matched");
    check(find_header_end(buf, 8) != NULL &&
              (size_t)(find_header_end(buf, 8) - buf) == 4,
          "header_end: terminator completed by second read");

    strcpy(buf, "no terminator here");
    check(find_header_end(buf, 17) == NULL, "header_end: missing terminator");

    check(find_header_end(buf, 3) == NULL, "header_end: too short to match");
}

/* ---------- tests for parse_request_line ---------- */

static void test_parse_request_line(void) {
    char method[MAX_METHOD_LEN];
    char path[MAX_PATH_LEN];

    check(parse_request_line("POST /notifications HTTP/1.1", method,
                             sizeof(method), path, sizeof(path)) &&
              strcmp(method, "POST") == 0 &&
              strcmp(path, "/notifications") == 0,
          "request_line: standard POST");

    check(parse_request_line("GET /notifications HTTP/1.1", method,
                             sizeof(method), path, sizeof(path)) &&
              strcmp(method, "GET") == 0,
          "request_line: GET method");

    /* Query string is stripped so routing still matches */
    check(parse_request_line("POST /notifications?debug=1 HTTP/1.1", method,
                             sizeof(method), path, sizeof(path)) &&
              strcmp(path, "/notifications") == 0,
          "request_line: query string stripped");

    /* HTTP/0.9 style with no version token */
    check(parse_request_line("GET /health", method, sizeof(method), path,
                             sizeof(path)) &&
              strcmp(path, "/health") == 0,
          "request_line: missing version tolerated");

    check(!parse_request_line("", method, sizeof(method), path, sizeof(path)),
          "request_line: empty rejected");

    check(
        !parse_request_line("POST", method, sizeof(method), path, sizeof(path)),
        "request_line: no target rejected");

    check(!parse_request_line(" /notifications HTTP/1.1", method,
                              sizeof(method), path, sizeof(path)),
          "request_line: empty method rejected");

    check(!parse_request_line("POST notifications HTTP/1.1", method,
                              sizeof(method), path, sizeof(path)),
          "request_line: relative target rejected");

    /* A path within the buffer is copied in full */
    check(parse_request_line("POST /a/very/long/path HTTP/1.1", method,
                             sizeof(method), path, sizeof(path)) &&
              strcmp(path, "/a/very/long/path") == 0,
          "request_line: long path accepted");

    /* A path past the buffer must be rejected, not overflowed */
    {
        char long_target[600];
        size_t n = 0;
        long_target[n++] = '/';
        memset(long_target + n, 'p', 500);
        long_target[501] = '\0';
        char line[700];
        snprintf(line, sizeof(line), "POST %s HTTP/1.1", long_target);
        check(!parse_request_line(line, method, sizeof(method), path,
                                  sizeof(path)),
              "request_line: overlong path rejected");
    }

    /* Method longer than the buffer must be rejected, not overflowed */
    check(!parse_request_line("VERYLONGMETHODNAME /notifications HTTP/1.1",
                              method, sizeof(method), path, sizeof(path)),
          "request_line: overlong method rejected");
}

/* ---------- tests for parse_content_length ---------- */

static void test_parse_content_length(void) {
    long value = 0;

    check(parse_content_length("POST /notifications HTTP/1.1\r\n\r\n",
                               &value) == CL_ABSENT,
          "content_length: absent");

    check(parse_content_length("POST / HTTP/1.1\r\nContent-Length: 18\r\n\r\n",
                               &value) == CL_PRESENT &&
              value == 18,
          "content_length: present");

    check(parse_content_length("POST / HTTP/1.1\r\ncontent-length: 42\r\n\r\n",
                               &value) == CL_PRESENT &&
              value == 42,
          "content_length: header name is case-insensitive");

    check(
        parse_content_length("POST / HTTP/1.1\r\nContent-Length:   7  \r\n\r\n",
                             &value) == CL_PRESENT &&
            value == 7,
        "content_length: surrounding whitespace tolerated");

    check(parse_content_length("POST / HTTP/1.1\r\nContent-Length: 0\r\n\r\n",
                               &value) == CL_PRESENT &&
              value == 0,
          "content_length: zero");

    check(parse_content_length(
              "POST / HTTP/1.1\r\nX-Other: 5\r\nContent-Length: 9\r\n\r\n",
              &value) == CL_PRESENT &&
              value == 9,
          "content_length: not confused by other headers");

    check(parse_content_length(
              "POST / HTTP/1.1\r\nContent-Type: application/json\r\n\r\n",
              &value) == CL_ABSENT,
          "content_length: content-type alone is absent");

    check(parse_content_length("POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n",
                               &value) == CL_MALFORMED,
          "content_length: non-numeric is malformed");

    check(parse_content_length("POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
                               &value) == CL_MALFORMED,
          "content_length: negative is malformed");

    check(parse_content_length("POST / HTTP/1.1\r\nContent-Length: \r\n\r\n",
                               &value) == CL_MALFORMED,
          "content_length: empty value is malformed");

    check(parse_content_length(
              "POST / HTTP/1.1\r\nContent-Length: 99999999999999999999\r\n\r\n",
              &value) == CL_MALFORMED,
          "content_length: overflow is malformed");
}

/* ---------- tests for request_handler_dispatch ---------- */

static server_config_t make_config(void) {
    server_config_t config;
    config.port = SERVER_PORT;
    config.telegram_config.token = "test-token";
    config.telegram_config.chat_id = "12345";
    return config;
}

/* Build a well-formed POST to /notifications with a matching Content-Length. */
static void dispatch_post(const server_config_t *config, const char *body,
                          http_response_t *response) {
    char headers[512];
    snprintf(headers, sizeof(headers),
             "POST /notifications HTTP/1.1\r\n"
             "Host: localhost\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "\r\n",
             strlen(body));
    request_handler_dispatch(config, headers, body, strlen(body), response);
}

static void dispatch_raw(const server_config_t *config, const char *headers,
                         const char *body, http_response_t *response) {
    request_handler_dispatch(config, headers, body, body ? strlen(body) : 0,
                             response);
}

static void test_dispatch(void) {
    server_config_t config = make_config();
    http_response_t response;

    /* --- routing (no Telegram call needed) --- */

    dispatch_raw(&config, "GET /notifications HTTP/1.1\r\n\r\n", "", &response);
    check(response.status_code == 405, "dispatch: GET /notifications is 405");

    dispatch_raw(&config, "GET /other HTTP/1.1\r\n\r\n", "", &response);
    check(response.status_code == 404, "dispatch: unknown path is 404");

    dispatch_raw(&config,
                 "PUT /notifications HTTP/1.1\r\n\r\nContent-Length: 0\r\n\r\n",
                 "", &response);
    check(response.status_code == 405, "dispatch: PUT /notifications is 405");

    dispatch_raw(
        &config,
        "POST /notifications?x=1 HTTP/1.1\r\nContent-Length: 0\r\n\r\n", "",
        &response);
    check(response.status_code == 400,
          "dispatch: query route with empty body is 400");

    /* --- framing errors --- */

    dispatch_raw(&config, "POST /notifications HTTP/1.1\r\n\r\n",
                 "{\"message\":\"hi\"}", &response);
    check(response.status_code == 411,
          "dispatch: missing Content-Length is 411");

    dispatch_raw(
        &config,
        "POST /notifications HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n", "",
        &response);
    check(response.status_code == 413,
          "dispatch: oversized Content-Length is 413");

    dispatch_raw(&config,
                 "POST /notifications HTTP/1.1\r\nContent-Length: abc\r\n\r\n",
                 "", &response);
    check(response.status_code == 400,
          "dispatch: malformed Content-Length is 400");

    dispatch_raw(&config, "garbage", "", &response);
    check(response.status_code == 400,
          "dispatch: unparsable request line is 400");

    dispatch_raw(&config, NULL, "", &response);
    check(response.status_code == 500, "dispatch: NULL headers is 500");

    /* --- JSON validation (all rejected before any Telegram call) --- */

    dispatch_post(&config, "{\"message\":\"hi\"}", &response);
    check(response.status_code == 502,
          "dispatch: valid message reaches Telegram (dead port -> 502)");

    dispatch_post(&config, "not json", &response);
    check(response.status_code == 400, "dispatch: invalid JSON is 400");

    dispatch_post(&config, "{\"foo\":\"bar\"}", &response);
    check(response.status_code == 400, "dispatch: missing message is 400");

    dispatch_post(&config, "{\"message\":\"\"}", &response);
    check(response.status_code == 400, "dispatch: empty message is 400");

    dispatch_post(&config, "{\"message\":\"hello\\u1F600\"}", &response);
    check(response.status_code == 400, "dispatch: \\uXXXX escape is 400");

    dispatch_post(&config, "", &response);
    check(response.status_code == 400, "dispatch: zero-length body is 400");

    /* Content-Length disagrees with the framed body length */
    dispatch_raw(&config,
                 "POST /notifications HTTP/1.1\r\nContent-Length: 5\r\n\r\n",
                 "{\"message\":\"hi\"}", &response);
    check(response.status_code == 400, "dispatch: length mismatch is 400");

    /* Response body shape */
    dispatch_post(&config, "bad", &response);
    check(strcmp(response.body,
                 "{\"status\": 400, \"message\": \"Bad Request\"}") == 0,
          "dispatch: response body format");
    check(response.body_length == strlen(response.body),
          "dispatch: body_length tracks body");
}

/* ---------- mock Telegram server (one connection per child) ---------- */

static void mock_telegram_run(int port, int status_code,
                              const char *response_body) {
    /* Never outlive the test run: if nothing connects we exit instead of
     * blocking the parent's waitpid() forever. */
    alarm(10);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
        _exit(1);

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((unsigned short)port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        _exit(1);
    if (listen(server_fd, 1) < 0)
        _exit(1);

    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0)
        _exit(1);

    char buf[8192];
    size_t total = 0;
    while (total < sizeof(buf) - 1) {
        ssize_t n = recv(client_fd, buf + total, sizeof(buf) - 1 - total, 0);
        if (n <= 0)
            break;
        total += (size_t)n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n"))
            break;
    }

    char response[2048];
    int len = snprintf(response, sizeof(response),
                       "HTTP/1.1 %d %s\r\n"
                       "Content-Type: application/json\r\n"
                       "Content-Length: %zu\r\n"
                       "Connection: close\r\n"
                       "\r\n"
                       "%s",
                       status_code, status_code == 200 ? "OK" : "Error",
                       strlen(response_body), response_body);
    if (len > 0)
        send(client_fd, response, (size_t)len, MSG_NOSIGNAL);

    close(client_fd);
    close(server_fd);
}

/* Start a one-shot mock Telegram server; returns its pid. */
static pid_t spawn_mock_telegram(int status_code, const char *response_body) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        mock_telegram_run(MOCK_TELEGRAM_PORT, status_code, response_body);
        _exit(0);
    }
    sleep_ms(100);
    return pid;
}

/* ---------- end-to-end helpers ---------- */

static ssize_t http_exchange(unsigned short port, const char *request,
                             size_t request_len, char *resp_out,
                             size_t resp_cap) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    size_t sent = 0;
    while (sent < request_len) {
        ssize_t n = send(fd, request + sent, request_len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        sent += (size_t)n;
    }
    shutdown(fd, SHUT_WR); /* deliver EOF to the server side */

    size_t total = 0;
    while (total + 1 < resp_cap) {
        ssize_t n = recv(fd, resp_out + total, resp_cap - 1 - total, 0);
        if (n <= 0)
            break;
        total += (size_t)n;
    }
    resp_out[total] = '\0';
    close(fd);
    return (ssize_t)total;
}

static int response_status(const char *resp) {
    if (strncmp(resp, "HTTP/1.1 ", 9) != 0)
        return -1;
    return atoi(resp + 9);
}

/* Single-shot exchange that reports what actually came back.
 * Returns the HTTP status, or -1 if the server closed without responding. */
static int exchange(const char *request, size_t len, char *resp, size_t cap) {
    ssize_t n = http_exchange(SERVER_PORT, request, len, resp, cap);
    if (n <= 0) {
        if (errors < 20)
            printf("   [diag] no response (recv=%zd)\n", n);
        return -1;
    }
    int status = response_status(resp);
    const char *body = strstr(resp, "\r\n\r\n");
    if (errors < 20) {
        printf("   [diag] status=%d body=<<%s>>\n", status,
               body ? body + 4 : "(none)");
    }
    return status;
}

/* Same as http_exchange but pauses between the header and body writes so the
 * server genuinely needs multiple recv() calls to frame the request. */
static ssize_t http_exchange_split(unsigned short port, const char *part1,
                                   size_t part1_len, const char *part2,
                                   size_t part2_len, char *resp_out,
                                   size_t resp_cap) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    if (send(fd, part1, part1_len, MSG_NOSIGNAL) != (ssize_t)part1_len) {
        close(fd);
        return -1;
    }
    sleep_ms(80);
    if (part2_len > 0 &&
        send(fd, part2, part2_len, MSG_NOSIGNAL) != (ssize_t)part2_len) {
        close(fd);
        return -1;
    }
    shutdown(fd, SHUT_WR);

    size_t total = 0;
    while (total + 1 < resp_cap) {
        ssize_t n = recv(fd, resp_out + total, resp_cap - 1 - total, 0);
        if (n <= 0)
            break;
        total += (size_t)n;
    }
    resp_out[total] = '\0';
    close(fd);
    return (ssize_t)total;
}

/* Same as exchange, but pauses between the two writes so the server must use
 * several recv() calls to frame the request. */
static int exchange_split(const char *part1, size_t part1_len,
                          const char *part2, size_t part2_len, char *resp,
                          size_t cap) {
    ssize_t n = http_exchange_split(SERVER_PORT, part1, part1_len, part2,
                                    part2_len, resp, cap);
    if (n <= 0) {
        if (errors < 20)
            printf("   [diag] no response (recv=%zd)\n", n);
        return -1;
    }
    int status = response_status(resp);
    const char *body = strstr(resp, "\r\n\r\n");
    if (errors < 20) {
        printf("   [diag] status=%d body=<<%s>>\n", status,
               body ? body + 4 : "(none)");
    }
    return status;
}

/* Mirror main.c's shutdown handler: clear g_keep_running so accept() unwinds.
 */
static void server_test_signal_handler(int sig) {
    (void)sig;
    g_keep_running = 0;
}

/* Run the server under test in a child process. */
static pid_t spawn_server(void) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = server_test_signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0; /* no SA_RESTART: accept() must return EINTR */
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);

        server_config_t config = make_config();
        _exit(request_handler_start(&config) == 0 ? 0 : 1);
    }
    sleep_ms(150);
    return pid;
}

static void test_end_to_end(void) {
    char response[4096];
    char request[2048];
    int len;
    int status;

    /*
     * The server child inherits the environment at fork time, so this must be
     * set before spawn_server(). With no mock listening on this port, Telegram
     * lookups fail and the service answers 502.
     */
    setenv("TELEGRAM_API_URL", "http://127.0.0.1:18932", 1);

    pid_t server_pid = spawn_server();

    /* --- happy path: 200 proxied from Telegram --- */

    pid_t mock = spawn_mock_telegram(200, "{\"ok\":true}");

    len = snprintf(request, sizeof(request),
                   "POST /notifications HTTP/1.1\r\n"
                   "Host: localhost\r\n"
                   "Content-Type: application/json\r\n"
                   "Content-Length: 19\r\n"
                   "\r\n"
                   "{\"message\":\"hello\"}");
    status = exchange(request, (size_t)len, response, sizeof(response));
    check(status == 200, "e2e: happy path is 200");
    check(strstr(response, "\"status\": 200") != NULL, "e2e: 200 body");
    /* {"status": 200, "message": "OK"} is 32 bytes */
    check(strstr(response, "Content-Length: 32") != NULL,
          "e2e: Content-Length matches body size");
    check(strstr(response, "Connection: close") != NULL,
          "e2e: Connection: close advertised");
    int child_status;
    waitpid(mock, &child_status, 0);

    /* --- Telegram error codes are proxied verbatim --- */

    mock = spawn_mock_telegram(
        429, "{\"ok\":false,\"description\":\"Too Many Requests\"}");
    status = exchange(request, (size_t)len, response, sizeof(response));
    check(status == 429, "e2e: Telegram 429 proxied");
    waitpid(mock, &child_status, 0);

    mock = spawn_mock_telegram(
        401, "{\"ok\":false,\"description\":\"Unauthorized\"}");
    status = exchange(request, (size_t)len, response, sizeof(response));
    check(status == 401, "e2e: Telegram 401 proxied");
    waitpid(mock, &child_status, 0);

    /* --- Telegram unreachable is 502 (no mock is listening now) --- */

    status = exchange(request, (size_t)len, response, sizeof(response));
    check(status == 502, "e2e: unreachable Telegram is 502");

    /* --- routing errors (still serving: proves the accept loop continues) ---
     */

    len = snprintf(request, sizeof(request),
                   "GET /notifications HTTP/1.1\r\n"
                   "Host: localhost\r\n\r\n");
    status = exchange(request, (size_t)len, response, sizeof(response));
    check(status == 405, "e2e: GET is 405");

    len = snprintf(request, sizeof(request),
                   "POST /other HTTP/1.1\r\n"
                   "Host: localhost\r\n"
                   "Content-Length: 19\r\n"
                   "\r\n"
                   "{\"message\":\"hello\"}");
    status = exchange(request, (size_t)len, response, sizeof(response));
    check(status == 404, "e2e: unknown path is 404");

    /* --- bad payloads --- */

    len = snprintf(request, sizeof(request),
                   "POST /notifications HTTP/1.1\r\n"
                   "Content-Length: 8\r\n"
                   "\r\n"
                   "not json");
    status = exchange(request, (size_t)len, response, sizeof(response));
    check(status == 400, "e2e: invalid JSON is 400");

    len = snprintf(request, sizeof(request),
                   "POST /notifications HTTP/1.1\r\n"
                   "Content-Length: 13\r\n"
                   "\r\n"
                   "{\"foo\":\"bar\"}");
    status = exchange(request, (size_t)len, response, sizeof(response));
    check(status == 400, "e2e: missing message is 400");

    len = snprintf(request, sizeof(request),
                   "POST /notifications HTTP/1.1\r\n"
                   "Content-Length: 13\r\n"
                   "\r\n"
                   "{\"message\":\"\"}");
    status = exchange(request, (size_t)len, response, sizeof(response));
    check(status == 400, "e2e: empty message is 400");

    /* --- missing Content-Length --- */

    len = snprintf(request, sizeof(request),
                   "POST /notifications HTTP/1.1\r\n"
                   "Host: localhost\r\n\r\n");
    status = exchange(request, (size_t)len, response, sizeof(response));
    check(status == 411, "e2e: missing Content-Length is 411");

    /* --- oversized body --- */

    len = snprintf(request, sizeof(request),
                   "POST /notifications HTTP/1.1\r\n"
                   "Content-Length: 99999999\r\n\r\n");
    status = exchange(request, (size_t)len, response, sizeof(response));
    check(status == 413, "e2e: oversized Content-Length is 413");

    /* A body of exactly MAX_HTTP_BODY bytes is accepted by the framing layer */
    {
        const char *head =
            "POST /notifications HTTP/1.1\r\nContent-Length: 16384\r\n\r\n";
        char big[MAX_HTTP_BODY + 64];
        size_t head_len = strlen(head);
        memcpy(big, head, head_len);
        memset(big + head_len, 'x', MAX_HTTP_BODY);
        status =
            exchange(big, head_len + MAX_HTTP_BODY, response, sizeof(response));
        check(status == 400,
              "e2e: max-size body reaches the JSON parser (400 for bad JSON)");
    }

    /* --- oversized headers --- */

    {
        char big[MAX_HEADER_SIZE + 512];
        size_t n = 0;
        big[n++] = 'G';
        big[n++] = 'E';
        big[n++] = 'T';
        big[n++] = ' ';
        big[n++] = '/';
        big[n++] = ' ';
        big[n++] = 'H';
        memset(big + n, 'x', MAX_HEADER_SIZE + 400);
        n += MAX_HEADER_SIZE + 400;
        status = exchange(big, n, response, sizeof(response));
        check(status == 431, "e2e: oversized headers are 431");
    }

    /* --- truncated body: connection dropped with no response --- */

    len = snprintf(request, sizeof(request),
                   "POST /notifications HTTP/1.1\r\n"
                   "Content-Length: 100\r\n"
                   "\r\n"
                   "{\"message\":");
    status = exchange(request, (size_t)len, response, sizeof(response));
    check(status == -1, "e2e: truncated body closes without a response");

    /* --- request split across TCP segments --- */

    mock = spawn_mock_telegram(200, "{\"ok\":true}");
    {
        const char *head = "POST /notifications HTTP/1.1\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: 19\r\n"
                           "\r\n";
        const char *body = "{\"message\":\"hello\"}";
        status = exchange_split(head, strlen(head), body, strlen(body),
                                response, sizeof(response));
        check(status == 200, "e2e: headers and body split across segments");
    }
    waitpid(mock, &child_status, 0);

    /* --- request line split from the rest of the headers --- */

    mock = spawn_mock_telegram(200, "{\"ok\":true}");
    {
        const char *part1 = "POST /notif";
        const char *part2 = "ications HTTP/1.1\r\nContent-Length: 19\r\n\r\n"
                            "{\"message\":\"hello\"}";
        status = exchange_split(part1, strlen(part1), part2, strlen(part2),
                                response, sizeof(response));
        check(status == 200, "e2e: request line split mid-token");
    }
    waitpid(mock, &child_status, 0);

    /* --- slow client: the server must not block forever on a stalled peer ---
     */

    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(SERVER_PORT);

        bool connected =
            fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
        check(connected, "e2e: slow client connects");

        if (connected) {
            /* Send a partial request, then hold the connection open and idle.
             */
            send(fd, "POST /notifications HTTP/1.1\r\n", 31, MSG_NOSIGNAL);

            struct timeval start, end;
            gettimeofday(&start, NULL);

            /* The server's SO_RCVTIMEO should drop us after SOCKET_TIMEOUT_SEC.
             */
            char drain[512];
            ssize_t n = recv(fd, drain, sizeof(drain), 0);
            gettimeofday(&end, NULL);

            double elapsed = (double)(end.tv_sec - start.tv_sec) +
                             (double)(end.tv_usec - start.tv_usec) / 1e6;
            printf("   [diag] slow client dropped after %.1fs\n", elapsed);
            check(n <= 0, "e2e: stalled connection is closed by the server");
            check(elapsed >= 4.0 && elapsed < 9.0,
                  "e2e: close happens near SOCKET_TIMEOUT_SEC");
        }
        if (fd >= 0)
            close(fd);
    }

    /* The server must still be serving after abandoning a slow client. */
    mock = spawn_mock_telegram(200, "{\"ok\":true}");
    len = snprintf(request, sizeof(request),
                   "POST /notifications HTTP/1.1\r\n"
                   "Content-Length: 19\r\n"
                   "\r\n"
                   "{\"message\":\"hello\"}");
    status = exchange(request, (size_t)len, response, sizeof(response));
    check(status == 200, "e2e: server still serving after a stalled client");
    waitpid(mock, &child_status, 0);

    /* --- graceful shutdown --- */

    kill(server_pid, SIGTERM);
    waitpid(server_pid, &child_status, 0);
    check(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0,
          "e2e: SIGTERM shuts down cleanly");

    /* Server is gone: a new connection must be refused */
    check(http_exchange(SERVER_PORT, request, 10, response, sizeof(response)) ==
              -1,
          "e2e: socket released after shutdown");
}

/* ---------- main ---------- */

int main(void) {
    /* Unbuffered so progress is visible when output is piped. */
    setvbuf(stdout, NULL, _IONBF, 0);

    curl_global_init(CURL_GLOBAL_DEFAULT);

    printf("=== find_header_end tests ===\n");
    test_find_header_end();

    printf("\n=== parse_request_line tests ===\n");
    test_parse_request_line();

    printf("\n=== parse_content_length tests ===\n");
    test_parse_content_length();

    printf("\n=== request_handler_dispatch tests ===\n");
    setenv("TELEGRAM_API_URL", "http://127.0.0.1:18933", 1);
    test_dispatch();

    printf("\n=== end-to-end tests ===\n");
    test_end_to_end();

    curl_global_cleanup();

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}

