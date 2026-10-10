#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "server.h"

#include <curl/curl.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROG_NAME "notification-service"

/* Clears g_keep_running (server.c watches it) to unwind the accept loop. */
static void shutdown_handler(int sig) {
    (void)sig;
    g_keep_running = 0;
}

/*
 * Strict decimal parser: the first character must be a digit so that values
 * such as " 8080" or "-1" are rejected rather than silently accepted by
 * strtoul(). Trailing garbage ("8080abc") is rejected too.
 */
static bool parse_port(const char *str, unsigned short *out_port) {
    if (str[0] < '0' || str[0] > '9') {
        return false;
    }

    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(str, &end, 10);

    if (errno != 0) { /* overflow */
        return false;
    }
    if (end == str) { /* no digits consumed */
        return false;
    }
    if (*end != '\0') { /* trailing garbage */
        return false;
    }
    if (value < 1UL || value > 65535UL) {
        return false;
    }

    *out_port = (unsigned short)value;
    return true;
}

/* Return the value of a required env var, or NULL if unset or empty. */
static const char *require_env(const char *name) {
    const char *value = getenv(name);
    if (!value || value[0] == '\0') {
        return NULL;
    }
    return value;
}

/* Append a name to a comma-separated "missing variables" list. */
static void note_missing(char *list, size_t capacity, const char *name) {
    size_t used = strlen(list);
    if (used + strlen(name) + 2 >= capacity) {
        return; /* list is full; the caller still reports what fit */
    }
    snprintf(list + used, capacity - used, "%s%s", used ? ", " : "", name);
}

static void usage(const char *reason) {
    if (reason) {
        fprintf(stderr, "error: %s\n\n", reason);
    }

    fprintf(stderr,
            "Usage: %s\n"
            "\n"
            "Required environment variables:\n"
            "  PORT             TCP port to listen on (1-65535)\n"
            "  TELEGRAM_TOKEN   Bot token issued by @BotFather\n"
            "  TELEGRAM_CHAT_ID Target chat id, or @channelusername\n"
            "\n"
            "Route:\n"
            "  POST %s   {\"message\": \"...\"}\n",
            PROG_NAME, NOTIFICATION_PATH);
}

int main(void) {
    /* Must precede every other libcurl entry point, including easy_init(). */
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        fprintf(stderr, "error: curl_global_init failed\n");
        return 1;
    }

    /* A peer closing mid-write must yield EPIPE, not a fatal signal. */
    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa = {.sa_handler = shutdown_handler};
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART — accept() must return EINTR */
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0) {
        perror("sigaction");
        curl_global_cleanup();
        return 1;
    }

    const char *port_str = require_env("PORT");
    const char *token = require_env("TELEGRAM_TOKEN");
    const char *chat_id = require_env("TELEGRAM_CHAT_ID");

    if (!port_str || !token || !chat_id) {
        char missing[128] = "";
        if (!port_str) {
            note_missing(missing, sizeof(missing), "PORT");
        }
        if (!token) {
            note_missing(missing, sizeof(missing), "TELEGRAM_TOKEN");
        }
        if (!chat_id) {
            note_missing(missing, sizeof(missing), "TELEGRAM_CHAT_ID");
        }

        char reason[192];
        snprintf(reason, sizeof(reason),
                 "missing required environment variable(s): %s", missing);
        usage(reason);
        curl_global_cleanup();
        return 1;
    }

    server_config_t config;
    memset(&config, 0, sizeof(config));

    if (!parse_port(port_str, &config.port)) {
        char reason[192];
        snprintf(reason, sizeof(reason),
                 "invalid PORT \"%s\": expected an integer in 1-65535",
                 port_str);
        usage(reason);
        curl_global_cleanup();
        return 1;
    }

    config.telegram_config.token = token;
    config.telegram_config.chat_id = chat_id;

    /* Blocks until g_keep_running is cleared by shutdown_handler(). */
    int rc = request_handler_start(&config);

    curl_global_cleanup();

    if (rc != 0) {
        fprintf(stderr, "%s: server failed to start or stopped with an error\n",
                PROG_NAME);
    }
    return (rc == 0) ? 0 : 1;
}
