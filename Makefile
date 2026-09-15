.DEFAULT_GOAL := all
# Makefile for the Scheduler project
#
# Builds src/scheduler.c (plus the translation units it depends on) into a
# single executable, and provides a `run` target to execute it.
#
# Usage:
#   make            # build ./scheduler using the default workload graph
#   make run        # build (if needed) then execute ./scheduler
#   make LOG=1      # enable worker logging (off by default; see conf.h WORKER_LOG)
#   make CASE=total_graph   # build against the combined total-graph case
#   make clean      # remove build artifacts and the executable

CC      ?= cc
CFLAGS  ?= -std=gnu11 -O2 -Wall -Wextra -pthread
CPPFLAGS += -D_GNU_SOURCE -Iinclude -Isrc -I.

# Which generated workload graph to compile in (see include/painter.h).
#   subgraph     -> cases/qwen3_14b_decode_subgraph.h   (PAINTER_THREAD_CNT=2)
#   total_graph  -> cases/qwen3_14b_decode_total_graph.h (PAINTER_THREAD_CNT=1)
CASE ?= subgraph

ifeq ($(CASE),subgraph)
SCHEDULER_CASE      := cases/qwen3_14b_decode_subgraph.h
PAINTER_THREAD_CNT  := 2
else ifeq ($(CASE),total_graph)
SCHEDULER_CASE      := cases/qwen3_14b_decode_total_graph.h
PAINTER_THREAD_CNT  := 1
else ifneq ($(filter $(CASE),qwen3_decode_layer_120 deepseek_v4_csa_a deepseek_v4_csa_b),)
SCHEDULER_CASE      := cases/$(CASE).h
PAINTER_THREAD_CNT  := 1
else
$(error unknown CASE '$(CASE)')
endif

# Worker logging: 0 = off (default), 1 = on. Override with: make LOG=1
LOG ?= 0

# SCHEDULER_CASE is stringized by include/painter.h, so pass it unquoted.
CPPFLAGS += -DSCHEDULER_CASE=$(SCHEDULER_CASE) -DPAINTER_THREAD_CNT=$(PAINTER_THREAD_CNT) -DWORKER_LOG=$(LOG)

TARGET := scheduler
SRCS   := src/scheduler.c src/dispatch.c src/painter.c src/log.c
BUILD_DIR := build/$(CASE)-log$(LOG)
OBJS   := $(SRCS:src/%.c=$(BUILD_DIR)/%.o)

.PHONY: all run clean FORCE check-captures

FORCE:

check-captures:
	python3 tools/gen_capture_cases.py --check
	python3 -m unittest discover -s tests -v

all: $(TARGET)

$(TARGET): $(OBJS) FORCE
	$(CC) $(CFLAGS) -o $@ $(OBJS)

$(BUILD_DIR)/%.o: src/%.c $(SCHEDULER_CASE) $(wildcard include/*.h) Makefile
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf build $(TARGET)
