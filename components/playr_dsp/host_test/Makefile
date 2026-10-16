# Host unit tests for the Playr output stage: make -C components/playr_dsp/host_test
# (or scripts/playr test). Uses the cJSON copy vendored with cspot.

CC      ?= cc
CJSON   := ../../spotify/cspot/bell/external/cJSON
BUILD   := build
SAN     ?= -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS  := -std=gnu99 -O2 -g -Wall -Wextra -Werror -Wno-missing-field-initializers -I../include -I$(CJSON) $(SAN)

SRCS    := ../dsp_core.c ../dsp_config.c test_dsp.c
OBJS    := $(patsubst %.c,$(BUILD)/%.o,$(notdir $(SRCS))) $(BUILD)/cJSON.o

.PHONY: test clean
test: $(BUILD)/test_dsp
	./$(BUILD)/test_dsp

$(BUILD)/test_dsp: $(OBJS)
	$(CC) $(SAN) $^ -lm -o $@

$(BUILD)/%.o: ../%.c ../include/*.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c ../include/*.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# third party: build quietly
$(BUILD)/cJSON.o: $(CJSON)/cJSON.c | $(BUILD)
	$(CC) -std=gnu99 -O2 -w $(SAN) -c $< -o $@

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
