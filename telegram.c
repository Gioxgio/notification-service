#include "telegram.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Response buffer

typedef struct {
    char *buffer;
    size_t capacity;
    size_t written;
} http_response_buf_t;

static size_t write_callback(void *contents, size_t size, size_t nmemb,
                             void *userp) {
    http_response_buf_t *buf = (http_response_buf_t *)userp;
    size_t total = size * nmemb;

    if (buf->buffer && buf->capacity > 0) {
        // Reserve 1 byte for null-termination
        size_t remaining = (buf->written < buf->capacity)
                               ? (buf->capacity - 1 - buf->written)
                               : 0;
        size_t to_copy = (total < remaining) ? total : remaining;
        memcpy(buf->buffer + buf->written, contents, to_copy);
        buf->written += to_copy;
        buf->buffer[buf->written] = '\0';
    }

    // Always return full total to satisfy libcurl contract
    return total;
}

// JSON escaping

static int json_escape_string(const char *input, char *output,
                              size_t output_size) {
    size_t j = 0;

    for (size_t i = 0; input[i] != '\0'; i++) {
        unsigned char c = (unsigned char)input[i];
        const char *replacement = NULL;
        char single[2];

        switch (c) {
        case '"':
            replacement = "\\\"";
            break;
        case '\\':
            replacement = "\\\\";
            break;
        case '\n':
            replacement = "\\n";
            break;
        case '\r':
            replacement = "\\r";
            break;
        case '\t':
            replacement = "\\t";
            break;
        case '\b':
            replacement = "\\b";
            break;
        case '\f':
            replacement = "\\f";
            break;
        default:
            if (c < 0x20) {
                /* Control characters — emit as \u00XX */
                if (output_size - j < 7)
                    return -1;
                snprintf(output + j, 7, "\\u%04x", c);
                j += 6;
                continue;
            }
            /* Regular character (including multi-byte UTF-8) */
            single[0] = (char)c;
            single[1] = '\0';
            replacement = single;
            break;
        }

        size_t len = strlen(replacement);
        if (output_size - j < len + 1)
            return -1;
        memcpy(output + j, replacement, len);
        j += len;
    }

    output[j] = '\0';
    return (int)j;
}

// Public API

int telegram_send_message(const telegram_config_t *config,
                          const notification_payload_t *payload,
                          char *response_out, size_t response_capacity) {
    CURL *curl;
    CURLcode res;
    long http_code = 0;
    int ret;

    /* Build URL: https://api.telegram.org/bot<token>/sendMessage */
    /* Allow override via TELEGRAM_API_URL for testing with a mock server */
    char url[512];
    const char *api_base = getenv("TELEGRAM_API_URL");
    if (api_base && api_base[0]) {
        snprintf(url, sizeof(url), "%s/bot%s/sendMessage", api_base,
                 config->token);
    } else {
        snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage",
                 config->token);
    }

    /* Build JSON body with escaped message */
    char escaped_msg[MAX_TELEGRAM_MSG_LEN * 2];
    if (json_escape_string(payload->message, escaped_msg, sizeof(escaped_msg)) <
        0) {
        return -1;
    }

    char json_body[MAX_TELEGRAM_MSG_LEN * 2 + 256];
    snprintf(json_body, sizeof(json_body),
             "{\"chat_id\": \"%s\", \"text\": \"%s\"}", config->chat_id,
             escaped_msg);

    /* Set up response buffer */
    http_response_buf_t resp_buf = {response_out, response_capacity, 0};
    if (response_out && response_capacity > 0) {
        response_out[0] = '\0';
    }

    /* Build headers */
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl = curl_easy_init();
    if (!curl) {
        curl_slist_free_all(headers);
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp_buf);

    res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
        return -1;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    ret = (int)http_code;

    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    return ret;
}
