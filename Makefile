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
	@echo "Preparing staging directory with kernel and grub.cfg..."
	@mkdir -p $(BUILD_DIR)/staging/boot/grub
	@./config-grub.sh $(OS_NAME) > $(BUILD_DIR)/staging/boot/grub/grub.cfg
	@cp $(BIN_DIR)/$(OS_BIN) $(BUILD_DIR)/staging/boot/
	@echo "Creating 1MB boot area..."
	@dd if=/dev/zero of=$(BUILD_DIR)/boot_area.img bs=1M count=1 status=none
	@echo "Creating 63MB ext2 partition from staging directory..."
	@dd if=/dev/zero of=$(BUILD_DIR)/partition.img bs=1M count=63 status=none
	@mke2fs -F -t ext2 -q -d $(BUILD_DIR)/staging $(BUILD_DIR)/partition.img
	@echo "Generating GRUB core image..."
	@grub-mkimage -O i386-pc -o $(BUILD_DIR)/core.img \
		-p '(hd0,msdos1)/boot/grub' \
		biosdisk part_msdos ext2 multiboot normal configfile
	@echo "Writing GRUB boot.img and core.img to boot area..."
	@dd if=/usr/lib/grub/i386-pc/boot.img of=$(BUILD_DIR)/boot_area.img bs=446 count=1 conv=notrunc status=none
	@dd if=$(BUILD_DIR)/core.img of=$(BUILD_DIR)/boot_area.img bs=512 seek=1 conv=notrunc status=none
	@echo "Concatenating boot area + partition → $@"
	@cat $(BUILD_DIR)/boot_area.img $(BUILD_DIR)/partition.img > $@
	@echo "Writing MBR partition table to final disk image..."
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