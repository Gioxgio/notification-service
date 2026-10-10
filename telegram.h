#ifndef TELEGRAM_H
#define TELEGRAM_H

#include "json.h"
#include <stddef.h>

typedef struct {
    const char *token;
    const char *chat_id;
} telegram_config_t;

int telegram_send_message(const telegram_config_t *config,
                          const notification_payload_t *payload,
                          char *response_out, size_t response_capacity);

#endif
