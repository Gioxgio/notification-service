CC = cc
CFLAGS = -Wall -Wextra -Werror -O2 -std=c11 -MMD -MP
LDFLAGS = -lcurl

BUILD_DIR = build

TARGET = $(BUILD_DIR)/notification-service

SRCS = main.c server.c telegram.c json.c
OBJS = $(SRCS:%.c=$(BUILD_DIR)/%.o)
DEPS = $(OBJS:.o=.d)

TEST_SRCS = tests/test_json.c tests/test_telegram.c tests/test_server.c
TEST_OBJS = $(TEST_SRCS:tests/%.c=$(BUILD_DIR)/%.o)
TEST_DEPS = $(TEST_OBJS:.o=.d)
TEST_CFLAGS = $(CFLAGS) -I.

TEST_JSON_BIN      = $(BUILD_DIR)/test_json
TEST_JSON_OBJS     = $(BUILD_DIR)/test_json.o $(BUILD_DIR)/json.o

TEST_TELEGRAM_BIN  = $(BUILD_DIR)/test_telegram
TEST_TELEGRAM_OBJS = $(BUILD_DIR)/test_telegram.o $(BUILD_DIR)/json.o

TEST_SERVER_BIN    = $(BUILD_DIR)/test_server
TEST_SERVER_OBJS   = $(BUILD_DIR)/test_server.o $(BUILD_DIR)/telegram.o $(BUILD_DIR)/json.o

TEST_BINS = $(TEST_JSON_BIN) $(TEST_TELEGRAM_BIN) $(TEST_SERVER_BIN)

all: $(TARGET) compile_commands.json

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: tests/%.c | $(BUILD_DIR)
	$(CC) $(TEST_CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $@

$(TEST_JSON_BIN): $(TEST_JSON_OBJS)
	$(CC) $^ -o $@ $(LDFLAGS)

$(TEST_TELEGRAM_BIN): $(TEST_TELEGRAM_OBJS)
	$(CC) $^ -o $@ $(LDFLAGS)

$(TEST_SERVER_BIN): $(TEST_SERVER_OBJS)
	$(CC) $^ -o $@ $(LDFLAGS)

test: $(TEST_BINS)
	$(TEST_JSON_BIN)
	$(TEST_TELEGRAM_BIN)
	$(TEST_SERVER_BIN)

# clangd/editor language server database. Without it, clangd falls back to
# guessing the language of headers as Objective-C++ ("-x objective-c++-header"),
# which conflicts with the -std=c11 below. Regenerate after changing any flag.
compile_commands.json: Makefile
	@rm -f $@.tmp
	@printf '[\n' > $@.tmp
	@first=1; \
	for f in $(SRCS) $(TEST_SRCS); do \
		case "$$f" in tests/*) flags='$(TEST_CFLAGS)' ;; *) flags='$(CFLAGS)' ;; esac; \
		[ $$first -eq 0 ] && printf ',\n' >> $@.tmp; \
		first=0; \
		cmd="$(CC) -c $$flags $$f"; \
		printf '  { "directory": "%s", "command": "%s", "file": "%s" }' \
			'$(CURDIR)' "$$cmd" "$$f" >> $@.tmp; \
	done; \
	printf '\n]\n' >> $@.tmp
	@mv $@.tmp $@

clean:
	rm -rf $(BUILD_DIR)
	rm -f *.o *.d notification-service test_json test_telegram test_server

-include $(DEPS) $(TEST_DEPS)

.PHONY: all clean test compile_commands.json
