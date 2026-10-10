#ifndef JSON_H
#define JSON_H

#include <stdbool.h>

#define MAX_TELEGRAM_MSG_LEN 4096

typedef struct {
    char message[MAX_TELEGRAM_MSG_LEN + 1];
} notification_payload_t;

bool json_parse_request(const char *json_raw,
                        notification_payload_t *out_payload);

#endif
