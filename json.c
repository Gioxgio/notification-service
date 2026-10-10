#include "json.h"
#include <ctype.h>
#include <string.h>

static bool skip_string(const char **p) {
    (*p)++; // skip opening quote
    while (**p && **p != '"') {
        if (**p == '\\') {
            (*p)++; // skip backslash
            if (**p == '\0')
                return false;
            if (**p == 'u') {
                // Skip \uXXXX
                (*p)++;
                for (int i = 0; i < 4; i++) {
                    if (!isxdigit((unsigned char)**p))
                        return false;
                    (*p)++;
                }
            } else {
                (*p)++; // skip escaped character
            }
        } else {
            (*p)++;
        }
    }
    if (**p != '"')
        return false;
    (*p)++; // skip closing quote
    return true;
}

static bool skip_value(const char **p) {
    while (**p && isspace((unsigned char)**p))
        (*p)++;

    if (**p == '\0')
        return false;

    if (**p == '"') {
        return skip_string(p);
    } else if (**p == '{') {
        (*p)++;
        while (**p && isspace((unsigned char)**p))
            (*p)++;
        if (**p == '}') {
            (*p)++;
            return true;
        }
        while (**p) {
            while (**p && isspace((unsigned char)**p))
                (*p)++;
            if (**p != '"')
                return false;
            if (!skip_string(p))
                return false;
            while (**p && isspace((unsigned char)**p))
                (*p)++;
            if (**p != ':')
                return false;
            (*p)++;
            if (!skip_value(p))
                return false;
            while (**p && isspace((unsigned char)**p))
                (*p)++;
            if (**p == ',') {
                (*p)++;
                continue;
            }
            if (**p == '}') {
                (*p)++;
                return true;
            }
            return false;
        }
        return false;
    } else if (**p == '[') {
        (*p)++;
        while (**p && isspace((unsigned char)**p))
            (*p)++;
        if (**p == ']') {
            (*p)++;
            return true;
        }
        while (**p) {
            if (!skip_value(p))
                return false;
            while (**p && isspace((unsigned char)**p))
                (*p)++;
            if (**p == ',') {
                (*p)++;
                continue;
            }
            if (**p == ']') {
                (*p)++;
                return true;
            }
            return false;
        }
        return false;
    } else {
        // number, true, false, null
        while (**p && **p != ',' && **p != '}' && **p != ']' &&
               !isspace((unsigned char)**p)) {
            (*p)++;
        }
        return true;
    }
}

bool json_parse_request(const char *json_raw,
                        notification_payload_t *out_payload) {
    if (!json_raw || !out_payload)
        return false;

    const char *p = json_raw;

    // Skip leading whitespace
    while (*p && isspace((unsigned char)*p))
        p++;

    // Expect '{'
    if (*p != '{')
        return false;
    p++;

    bool found_message = false;

    // Skip whitespace after '{'
    while (*p && isspace((unsigned char)*p))
        p++;

    // Empty object: no message field
    if (*p == '}')
        return false;

    while (*p) {
        // Skip whitespace
        while (*p && isspace((unsigned char)*p))
            p++;

        // Expect '"' for key
        if (*p != '"')
            return false;
        p++;

        // Read key (no escapes supported in keys)
        char key[64];
        size_t key_len = 0;
        while (*p && *p != '"') {
            if (*p == '\\')
                return false; // escape in key not supported
            if (key_len < sizeof(key) - 1) {
                key[key_len++] = *p;
            }
            p++;
        }
        if (*p != '"')
            return false; // unterminated key
        p++;
        key[key_len] = '\0';

        // Skip whitespace
        while (*p && isspace((unsigned char)*p))
            p++;

        // Expect ':'
        if (*p != ':')
            return false;
        p++;

        // Skip whitespace
        while (*p && isspace((unsigned char)*p))
            p++;

        if (strcmp(key, "message") == 0) {
            // Expect '"' for string value
            if (*p != '"')
                return false;
            p++;

            // Read and unescape value
            size_t msg_len = 0;
            while (*p && *p != '"') {
                if (*p == '\\') {
                    p++;
                    if (*p == '\0')
                        return false;
                    char escaped = *p;
                    p++;
                    switch (escaped) {
                    case '"':
                        if (msg_len < MAX_TELEGRAM_MSG_LEN)
                            out_payload->message[msg_len++] = '"';
                        break;
                    case '\\':
                        if (msg_len < MAX_TELEGRAM_MSG_LEN)
                            out_payload->message[msg_len++] = '\\';
                        break;
                    case 'n':
                        if (msg_len < MAX_TELEGRAM_MSG_LEN)
                            out_payload->message[msg_len++] = '\n';
                        break;
                    case 'r':
                        if (msg_len < MAX_TELEGRAM_MSG_LEN)
                            out_payload->message[msg_len++] = '\r';
                        break;
                    case 't':
                        if (msg_len < MAX_TELEGRAM_MSG_LEN)
                            out_payload->message[msg_len++] = '\t';
                        break;
                    case '/':
                        if (msg_len < MAX_TELEGRAM_MSG_LEN)
                            out_payload->message[msg_len++] = '/';
                        break;
                    case 'u':
                        return false; // \uXXXX not supported
                    default:
                        return false; // invalid escape
                    }
                } else {
                    // Regular character (including UTF-8 bytes > 0x7F)
                    if (msg_len >= MAX_TELEGRAM_MSG_LEN)
                        return false;
                    out_payload->message[msg_len++] = *p++;
                }
            }
            if (*p != '"')
                return false; // unterminated string
            p++;
            out_payload->message[msg_len] = '\0';

            // Reject empty message
            if (msg_len == 0)
                return false;

            found_message = true;
        } else {
            // Skip non-message values (strings, numbers, objects, arrays, etc.)
            if (!skip_value(&p))
                return false;
        }

        // Skip whitespace
        while (*p && isspace((unsigned char)*p))
            p++;

        if (*p == ',') {
            p++;
            continue;
        } else if (*p == '}') {
            p++;
            break;
        } else {
            return false;
        }
    }

    // Skip trailing whitespace
    while (*p && isspace((unsigned char)*p))
        p++;

    // Reject trailing content after closing brace
    if (*p != '\0')
        return false;

    return found_message;
}
