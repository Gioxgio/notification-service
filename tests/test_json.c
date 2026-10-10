#include "json.h"
#include <stdio.h>
#include <string.h>

static int passed = 0;
static int failed = 0;

static void check(bool condition, const char *name) {
    if (condition) {
        printf("PASS: %s\n", name);
        passed++;
    } else {
        printf("FAIL: %s\n", name);
        failed++;
    }
}

int main() {
    notification_payload_t payload;

    // --- Basic parsing ---
    check(json_parse_request("{\"message\": \"hello\"}", &payload) &&
              strcmp(payload.message, "hello") == 0,
          "Simple message");

    check(json_parse_request("  {  \"message\"  :  \"hi\"  }  ", &payload) &&
              strcmp(payload.message, "hi") == 0,
          "Whitespace");

    check(json_parse_request(
              "{\"chat_id\": 123, \"message\": \"test\", \"other\": true}",
              &payload) &&
              strcmp(payload.message, "test") == 0,
          "Multiple fields");

    // --- Escape sequences ---
    check(json_parse_request("{\"message\": \"hello\\nworld\"}", &payload) &&
              strcmp(payload.message, "hello\nworld") == 0,
          "Escape \\n");

    check(json_parse_request("{\"message\": \"\\\"\\\\\\n\\r\\t\\/\"}",
                             &payload) &&
              strcmp(payload.message, "\"\\\n\r\t/") == 0,
          "All escape types");

    check(json_parse_request("{\"message\": \"a\\tb\\rc\"}", &payload) &&
              strcmp(payload.message, "a\tb\rc") == 0,
          "Escapes \\t and \\r");

    // --- UTF-8 ---
    check(json_parse_request("{\"message\": \"hello\xF0\x9F\x98\x80\"}",
                             &payload) &&
              strcmp(payload.message, "hello\xF0\x9F\x98\x80") == 0,
          "Raw UTF-8 emoji");

    check(json_parse_request("{\"message\": \"caf\xC3\xA9\"}", &payload) &&
              strcmp(payload.message, "caf\xC3\xA9") == 0,
          "Raw UTF-8 accented");

    // --- Nested structures in other fields ---
    check(
        json_parse_request(
            "{\"data\": {\"nested\": true}, \"message\": \"hi\"}", &payload) &&
            strcmp(payload.message, "hi") == 0,
        "Nested object skipped");

    check(json_parse_request("{\"data\": [1, 2, 3], \"message\": \"hi\"}",
                             &payload) &&
              strcmp(payload.message, "hi") == 0,
          "Array skipped");

    check(json_parse_request(
              "{\"data\": {\"message\": \"nested\"}, \"message\": \"top\"}",
              &payload) &&
              strcmp(payload.message, "top") == 0,
          "Nested message field ignored");

    check(json_parse_request(
              "{\"data\": \"hello\\u1F600\", \"message\": \"hi\"}", &payload) &&
              strcmp(payload.message, "hi") == 0,
          "\\uXXXX in other field skipped");

    // --- Rejections ---
    check(!json_parse_request("{\"foo\": \"bar\"}", &payload),
          "Missing message field");

    check(!json_parse_request("{\"message\": \"\"}", &payload),
          "Empty message");

    check(!json_parse_request("{\"message\": \"hello\\u1F600\"}", &payload),
          "\\uXXXX in message rejected");

    check(!json_parse_request("not json", &payload), "Invalid JSON");

    check(!json_parse_request("", &payload), "Empty string");

    check(!json_parse_request("   ", &payload), "Whitespace only");

    check(!json_parse_request("{\"message\": \"hi\"} extra", &payload),
          "Trailing content rejected");

    check(!json_parse_request("{\"message\": 123}", &payload),
          "Number value rejected");

    check(!json_parse_request("{\"message\": null}", &payload),
          "Null value rejected");

    check(!json_parse_request("{\"message\": true}", &payload),
          "Boolean value rejected");

    check(!json_parse_request("{\"message\": {\"nested\": true}}", &payload),
          "Object value rejected");

    check(!json_parse_request("{\"message\": [1, 2, 3]}", &payload),
          "Array value rejected");

    check(!json_parse_request("{\"message\": \"unterminated}", &payload),
          "Unterminated string rejected");

    check(!json_parse_request("{\"message\": \"bad\\xescape\"}", &payload),
          "Invalid escape rejected");

    // --- Length boundary ---
    char buf[5000];

    // Exactly 4096 chars — should pass
    strcpy(buf, "{\"message\": \"");
    for (int i = 0; i < 4096; i++)
        strcat(buf, "a");
    strcat(buf, "\"}");
    check(json_parse_request(buf, &payload) && strlen(payload.message) == 4096,
          "Exactly 4096 chars accepted");

    // 4097 chars — should fail
    strcpy(buf, "{\"message\": \"");
    for (int i = 0; i < 4097; i++)
        strcat(buf, "a");
    strcat(buf, "\"}");
    check(!json_parse_request(buf, &payload), "4097 chars rejected");

    // 2048 \n escapes (raw 4096 chars, unescaped 2048) — should pass
    strcpy(buf, "{\"message\": \"");
    for (int i = 0; i < 2048; i++)
        strcat(buf, "\\n");
    strcat(buf, "\"}");
    check(json_parse_request(buf, &payload) && strlen(payload.message) == 2048,
          "2048 \\n escapes accepted");

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
