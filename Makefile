CC       ?= gcc
CFLAGS   := -std=c11 -Wall -Wextra -Wpedantic -O2 -g
CPPFLAGS := -Iinclude -D_XOPEN_SOURCE=700
LDFLAGS  :=

BUILD_DIR := build
SRC_DIR   := src
INC_DIR   := include
WORK_DIR  := workers

TARGET := watchdog
WORKER := worker

SRCS := $(SRC_DIR)/main.c \
        $(SRC_DIR)/signal_manager.c \
        $(SRC_DIR)/process_manager.c \
        $(SRC_DIR)/logger.c

OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))
HEADERS := $(INC_DIR)/logger.h $(INC_DIR)/signal_manager.h $(INC_DIR)/process_manager.h

.PHONY: all clean test re

all: $(TARGET) $(WORKER)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

$(WORKER): $(WORK_DIR)/worker.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LDFLAGS)

test: all
	@chmod +x tests/test_scenarios.sh
	@./tests/test_scenarios.sh

clean:
	rm -rf $(BUILD_DIR) $(TARGET) $(WORKER) watchdog.log

re: clean all
