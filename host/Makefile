# Host build: unit tests for the portable core.
CORE := ../components/core
CFLAGS := -std=c11 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
          -I$(CORE)/include -Icommon
SRCS := $(wildcard $(CORE)/src/*.c) common/flash_emu.c $(wildcard tests/*.c)
OUT := build

.PHONY: test clean
test: $(OUT)/tests
	./$(OUT)/tests

$(OUT)/tests: $(SRCS) $(wildcard $(CORE)/include/*.h) $(wildcard tests/*.h) common/flash_emu.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(SRCS) -lm -o $@

clean:
	rm -rf $(OUT)

# Bench emulator: real core + simulated sensors behind an emulated UART-to-Ethernet bridge.
EMU_SRCS := $(wildcard $(CORE)/src/*.c) common/flash_emu.c emu/emu.c
EMU_CFLAGS := -std=c11 -Wall -Wextra -Werror -g -O1 -I$(CORE)/include -Icommon -pthread

.PHONY: emu
emu: $(OUT)/emu

$(OUT)/emu: $(EMU_SRCS) $(wildcard $(CORE)/include/*.h) common/flash_emu.h
	@mkdir -p $(OUT)
	$(CC) $(EMU_CFLAGS) $(EMU_SRCS) -lm -o $@
