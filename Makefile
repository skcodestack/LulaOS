OS_ARCH := x86

BUILD_DIR = build
KERNEL_DIR := kernel
OBJECT_DIR := $(BUILD_DIR)/obj
BIN_DIR := $(BUILD_DIR)/bin
ISO_DIR := $(BUILD_DIR)/iso
ISO_BOOT_DIR := $(ISO_DIR)/boot
ISO_GRUB_DIR := $(ISO_BOOT_DIR)/grub


INCLUDES_DIR := includes
INCLUDES := $(patsubst %, -I% ,$(INCLUDES_DIR)) 

OS_NAME := LulaOS
OS_BIN := $(OS_NAME).bin
OS_IOS := $(OS_NAME).iso

CC := x86_64-elf-gcc
AS := x86_64-elf-as

O := -O3
W := -Wall -Wextra -Werror
CFLAGS := -m32 -std=gnu99 -ffreestanding ${O} ${W}  #-fno-stack-protector -fno-builtin -fno-exceptions -fno-rtti -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-threadsafe-statics -fno-strict-aliasing -fno-omit-frame-pointer -fno-zero-initialized-in-bss -fno-stack-clash-protection -fno-stack-protector -f
LDFLAGS := -ffreestanding ${O} -nostdlib -lgcc # -T link.ld  -nostdlib -nodefaultlibs -nostartfiles -lgcc

SOURCES := $(shell lfind -name "*.[cS]")
SRC := ${patsubst ./%, ${OBJECT_DIR}/%, ${SOURCES}} 

$(OBJECT_DIR):
	@mkdir -p $@

$(BIN_DIR):
	@mkdir -p $@

$(ISO_DIR):
	@mkdir -p $@
	@mkdir -p $(ISO_BOOT_DIR)
	@mkdir -p $(ISO_GRUB_DIR)

$(OBJECT_DIR)/%.S.o: %.S 
	@mkdir -p $(@D)
	$(AS) $< -o $@

$(OBJECT_DIR)/%.c.o: %.c
	@mkdir -p $(@D)
	$(CC) $(INCLUDES) -c $< -o $@ $(CFLAGS)

$(BIN_DIR)/$(OS_BIN):$(OBJECT_DIR) $(BIN_DIR) $(SRC)
	echo "Linking $(SRC)..."
	$(CC) -T linder.lds -o $@  $(SRC) $(LDFLAGS)

$(BUILD_DIR)/$(OS_IOS): $(ISO_DIR) $(BIN_DIR)/$(OS_BIN) GRUB_TEMPLATE
	@./config-grub.sh $(OS_NAME) > $(ISO_GRUB_DIR) $(ISO_GRUB_DIR)/grub.cfg
	@cp $(BIN_DIR)/$(OS_BIN) $(ISO_BOOT_DIR)
	# @grub-mkrescue -o $(BUILD_DIR)/$(OS_IOS) $(ISO_DIR)

all: clean $(BUILD_DIR)/$(OS_IOS)

all-debug: O := -O0
all-debug: CFLAGS := -m32 -g -std=gnu99 -ffreestanding $(O) $(W) -fomit-frame-pointer
all-debug: LDFLAGS :=  -ffreestanding $(O)   -nostdlib -lgcc
all-debug: clean $(BUILD_DIR)/$(OS_IOS)
	@x86_64-elf-objdump -D $(BIN_DIR)/$(OS_BIN) > dump


clean : 
	@rm -rf $(BUILD_DIR)

run: $(BUILD_DIR)/$(OS_IOS)
	@qemu-system-x86_64 -cdrom $(BUILD_DIR)/$(OS_IOS)

debug-qemu: all-debug
	@objcopy --only-keep-debug $(BIN_DIR)/$(OS_BIN) $(BUILD_DIR)/kernel.dbg
	@qemu-system-x86_64 -s -S -kernel $(BIN_DIR)/$(OS_BIN) &
	@gdb -s $(BUILD_DIR)/kernel.dbg -ex "target remote localhost:1234"

debug-bochs: all-debug
	@bochsdbg -q -f ./bochsrc.bxrc 	




# BUILD_DIR = ./build
# SRC = ./src 

# $(BUILD_DIR/%.o): $(SRC/%.asm)
# 	x86_64-elf-gcc  -m32  -c $< -o $@ 

# $(BUILD_DIR/boot/%.bin): $(SRC/boot/%.asm)
# 	$(shell mkdir -p $(@D))
# 	nasm -f bin $< -o $@ 

# .PHONY: master
# master: $(BUILD_DIR)/boot/boot.bin
# 	dd if=$(BUILD_DIR)/boot/boot.bin  of=master.img bs=512 count=1 conv=notrunc
	

# .PHONY: bochs
# bochs: master
# 	bochsdbg -q -f ./bochsrc.bxrc 

# .PHONY: qemu
# qemu: master
# 	qemu-system-x86_64w -m 128M -drive file=master.img,index=0,media=disk,format=raw