# Toolchain
CC = arm-none-eabi-gcc

SRC_DIR   = src
BUILD_DIR = build

SRCS = $(SRC_DIR)/startup.c \
       $(SRC_DIR)/main.c \
       $(SRC_DIR)/rtos.c \
       $(SRC_DIR)/uart.c

OBJS = $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

LDSCRIPT = $(SRC_DIR)/linker.ld

CFLAGS = -mcpu=cortex-m3 -mthumb \
         -O0 -g3 -Wall \
         -ffreestanding -nostdlib -nostartfiles


LDFLAGS = -T $(LDSCRIPT) \
          -nostdlib \
          -Wl,--gc-sections \
          -lgcc

TARGET = $(BUILD_DIR)/rtos.elf

all: $(TARGET)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(TARGET) $(LDFLAGS)

clean:
	rm -rf $(BUILD_DIR)
