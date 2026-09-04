include config/make-debug-tool

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
OS_ISO := $(OS_NAME).iso
OS_IMG := $(OS_NAME).img
DISK_SIZE := 64M

CC := i686-elf-gcc
AS := i686-elf-as

O := -O3
W := -Wall -Wextra -Wextra
CFLAGS :=  -std=gnu99 -ffreestanding ${O} ${W}  #-fno-stack-protector -fno-builtin -fno-exceptions -fno-rtti -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-threadsafe-statics -fno-strict-aliasing -fno-omit-frame-pointer -fno-zero-initialized-in-bss -fno-stack-clash-protection -fno-stack-protector -f
LDFLAGS := -ffreestanding ${O} -nostdlib -lgcc # -T link.ld  -nostdlib -nodefaultlibs -nostartfiles -lgcc

SOURCES_FILES := $(shell find -name "*.[cS]")
SRC := ${patsubst ./%, ${OBJECT_DIR}/%.o, ${SOURCES_FILES}} 

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
	$(CC) $(INCLUDES) -c $< -o $@

$(OBJECT_DIR)/%.c.o: %.c
	@mkdir -p $(@D)
	$(CC) $(INCLUDES) -c $< -o $@ $(CFLAGS)

$(BIN_DIR)/$(OS_BIN):$(OBJECT_DIR) $(BIN_DIR) $(SRC)
	echo "Linking $(SRC)..."
	$(CC) -T linker.lds -o $@  $(SRC) $(LDFLAGS)

$(BUILD_DIR)/$(OS_ISO): $(ISO_DIR) $(BIN_DIR)/$(OS_BIN) GRUB_TEMPLATE
	@./config-grub.sh $(OS_NAME) >  $(ISO_GRUB_DIR)/grub.cfg
	@cp $(BIN_DIR)/$(OS_BIN) $(ISO_BOOT_DIR)
	@grub-mkrescue -o $(BUILD_DIR)/$(OS_ISO) $(ISO_DIR)

$(BUILD_DIR)/$(OS_IMG): $(BIN_DIR)/$(OS_BIN) GRUB_TEMPLATE
	@mkdir -p $(BUILD_DIR)
	@echo "Creating $(DISK_SIZE) disk image..."
	@dd if=/dev/zero of=$@ bs=1M count=64 status=none
	@echo "Formatting partition area as ext2 (offset 1MB)..."
	@mke2fs -F -t ext2 -E offset=1048576 -q $@
	@echo "Generating GRUB core image..."
	@mkdir -p $(BUILD_DIR)/boot/grub
	@./config-grub.sh $(OS_NAME) > $(BUILD_DIR)/boot/grub/grub.cfg
	@grub-mkimage -O i386-pc -o $(BUILD_DIR)/core.img \
		-p '(hd0,msdos1)/boot/grub' \
		biosdisk part_msdos ext2 multiboot normal
	@echo "Writing GRUB boot.img and core.img to disk..."
	@dd if=/usr/lib/grub/i386-pc/boot.img of=$@ bs=446 count=1 conv=notrunc status=none
	@dd if=$(BUILD_DIR)/core.img of=$@ bs=512 seek=1 conv=notrunc status=none
	@echo "Writing kernel and grub.cfg to ext2 partition..."
	@debugfs -w -R "mkdir /boot" $@ 2>/dev/null || true
	@debugfs -w -R "mkdir /boot/grub" $@ 2>/dev/null || true
	@debugfs -w -R "write $(CURDIR)/$(BIN_DIR)/$(OS_BIN) /boot/$(OS_BIN)" $@ 2>/dev/null
	@debugfs -w -R "write $(CURDIR)/$(BUILD_DIR)/boot/grub/grub.cfg /boot/grub/grub.cfg" $@ 2>/dev/null
	@echo "Creating MBR partition table..."
	@echo "start=2048, type=83, bootable" | sfdisk -q $@
	@echo "Disk image $@ ready"

all: clean $(BUILD_DIR)/$(OS_IMG)

all-debug: O := -O0
all-debug: CFLAGS := -m32 -g -std=gnu99 -ffreestanding $(O) $(W) -fomit-frame-pointer
all-debug: LDFLAGS :=  -ffreestanding $(O)   -nostdlib -lgcc
all-debug: clean $(BUILD_DIR)/$(OS_IMG)
	@echo "Dumping the disassembled kernel code to $(BUILD_DIR)/kdump.txt"
	@i686-elf-objdump -D $(BIN_DIR)/$(OS_BIN) > $(BUILD_DIR)/kdump.txt


clean : 
	sudo rm -rf $(BUILD_DIR)

run-qemu: $(BUILD_DIR)/$(OS_IMG)
	@qemu-system-i386 -hda $(BUILD_DIR)/$(OS_IMG) -m 2048 -smp 4 -monitor telnet::$(QEMU_MON_PORT),server,nowait &
	@sleep 1
	@telnet 127.0.0.1 $(QEMU_MON_PORT)

debug-qemu: all-debug
	@i686-elf-objcopy --only-keep-debug $(BIN_DIR)/$(OS_BIN) $(BUILD_DIR)/kernel.dbg
	@qemu-system-i386 -s -S -hda $(BUILD_DIR)/$(OS_IMG) -m 2048 -smp 4 -monitor telnet::$(QEMU_MON_PORT),server,nowait &
	@sleep 1
	@$(QEMU_MON_TERM) -e "telnet 127.0.0.1 $(QEMU_MON_PORT)"
	@gdb -s $(BUILD_DIR)/kernel.dbg -ex "target remote localhost:1234"

debug-bochs: all-debug
	@bochs -q -f ./bochs.cfg 	




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