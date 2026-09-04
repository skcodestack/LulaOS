# LulaOS 磁盘镜像分层布局与构建流程

## 最终磁盘镜像结构（LulaOS.img，64MB）

```
偏移地址         扇区号        内容
────────────────────────────────────────────────────────────────────
0x000000        LBA 0         ┌─────────────────────────────────┐
                              │ MBR 引导代码 (boot.img, 446 字节) │  ← BIOS 加载到 0x7C00 并执行
0x0001BE                      │ MBR 分区表 (64 字节, 4×16B)      │  ← sfdisk 写入
0x0001FE                      │ 引导签名 0x55 0xAA (2 字节)      │  ← BIOS 验证此值才允许引导
0x000200        LBA 1         ├─────────────────────────────────┤
                              │ core.img (grub-mkimage 生成)      │  ← boot.img 跳转到此处
                              │  内嵌模块:                        │
                              │    biosdisk   (BIOS INT 13h)      │
                              │    part_msdos (MBR 分区表解析)     │
                              │    ext2       (ext2 文件系统驱动)  │
                              │    multiboot  (Multiboot 协议加载) │
                              │    normal     (菜单模式)           │
                              │    configfile (配置文件解析)       │
                              │  内嵌 prefix:                     │
                              │    (hd0,msdos1)/boot/grub         │
                              │  (安全区域，无文件系统数据)        │
                              │                                   │
                              ... (core.img 通常 20~30KB)         │
                              │                                   │
0x100000 (1MB)  LBA 2048      ├─────────────────────────────────┤  ← 分区起始 (msdos1)
                              │ ext2 文件系统 (63MB)              │
                              │                                   │
                              │  超级块 (offset +1024)            │
                              │  块组描述符表                      │
                              │  inode 位图 / 块位图              │
                              │                                   │
                              │  /boot/                           │
                              │    ├── LulaOS.bin    (内核 ELF)   │
                              │    └── grub/                      │
                              │         └── grub.cfg  (启动菜单)  │
                              │                                   │
                              │  /lost+found/                     │
                              │                                   │
0x4000000 (64MB) LBA 131072   └─────────────────────────────────┘  ← 镜像结束
```

---

## GRUB 完整引导链条

```
BIOS (0xFFFF0 复位向量)
  │
  │  扫描 ATA 设备，读第一个扇区 (512B)
  │  验证偏移 510-511 == 0x55AA
  │  加载到物理地址 0x7C00，跳转执行
  ▼
boot.img (0x7C00, 446 字节)
  │  功能极简：只够跳转到 core.img 的起始扇区
  │  不含任何文件系统驱动，靠内嵌 LBA 地址硬跳转
  │  使用 BIOS INT 13h 读磁盘（实模式）
  ▼
core.img (LBA 1, 32位保护模式)
  │  初始化内嵌模块（biosdisk/part_msdos/ext2）
  │  根据内嵌 prefix 定位 (hd0,msdos1)/boot/grub
  │  加载 normal 模块 → 进入菜单模式
  │  读取 /boot/grub/grub.cfg
  ▼
grub.cfg 执行
  │  set gfxpayload=1024x768x32
  │  menuentry "LulaOS" {
  │      multiboot /boot/LulaOS.bin   ← 通过 Multiboot 协议加载 ELF
  │  }
  ▼
LulaOS 内核启动
  接收 Multiboot 信息结构体 (magic=0x2BADB002)
  从 entry.S 开始执行
```

---

## 构建步骤与命令详解

### 步骤 1：准备 staging 目录（文件系统内容源）

```makefile
@mkdir -p build/staging/boot/grub
@./config-grub.sh LulaOS > build/staging/boot/grub/grub.cfg
@cp build/bin/LulaOS.bin build/staging/boot/
```

**说明**：
- 在 `build/staging/` 中搭建与 ext2 分区根目录完全一致的目录树
- `grub.cfg` 由 `config-grub.sh` 对 `GRUB_TEMPLATE` 做变量替换生成
- 后续 `mke2fs -d` 会将此目录树完整"烧入"ext2 文件系统

**生成的 grub.cfg 内容**：
```
set gfxpayload=1024x768x32

menuentry "LulaOS" {
    multiboot /boot/LulaOS.bin
}
```

---

### 步骤 2：创建 1MB boot area（MBR + GRUB 安全区）

```makefile
@dd if=/dev/zero of=build/boot_area.img bs=1M count=1 status=none
```

**说明**：
- 1MB = 2048 扇区，对应 MBR 到第一个分区之间的"MBR gap"
- 此区域存放 `boot.img`（前 446 字节）和 `core.img`（扇区 1 起）
- 与 ext2 分区完全隔离，不受文件系统操作影响

---

### 步骤 3：创建带内容的 63MB ext2 分区

```makefile
@dd if=/dev/zero of=build/partition.img bs=1M count=63 status=none
@mke2fs -F -t ext2 -q -d build/staging build/partition.img
```

**关键命令 `mke2fs -d`**：
- `-F`：强制覆盖已有文件
- `-t ext2`：文件系统类型
- `-q`：静默模式
- `-d build/staging`：**将目录内容直接写入文件系统**（替代不可靠的 `debugfs`）

> `mke2fs -d` 是 e2fsprogs 1.43+（2015）引入的官方选项，在格式化时同步写入文件，
> inode、数据块、目录项一次性完整写入，无中间状态，不会出现 `debugfs` 的静默失败。

---

### 步骤 4：生成 GRUB core.img

```makefile
@grub-mkimage -O i386-pc -o build/core.img \
    -p '(hd0,msdos1)/boot/grub' \
    biosdisk part_msdos ext2 multiboot normal configfile
```

**各参数含义**：

| 参数 | 说明 |
|------|------|
| `-O i386-pc` | 目标平台（32 位 BIOS PC） |
| `-p '(hd0,msdos1)/boot/grub'` | 内嵌 prefix，GRUB 在此寻找 grub.cfg 和模块 |
| `biosdisk` | BIOS INT 13h 磁盘访问驱动 |
| `part_msdos` | MBR 分区表解析模块 |
| `ext2` | ext2/ext3/ext4 文件系统驱动 |
| `multiboot` | Multiboot 协议内核加载器 |
| `normal` | 菜单模式（读取 grub.cfg，显示菜单） |
| `configfile` | 配置文件命令解析 |

**core.img 内部结构**：
```
┌─────────────────────────────┐
│ 头部 (magic, 版本, 大小)     │
├─────────────────────────────┤
│ 内嵌模块 (按顺序排列)         │
│  ┌─ biosdisk.mod            │
│  ├─ part_msdos.mod          │
│  ├─ ext2.mod                │
│  ├─ multiboot.mod           │
│  ├─ normal.mod              │
│  └─ configfile.mod          │
├─────────────────────────────┤
│ prefix 字符串 (null 结尾)    │
│ "(hd0,msdos1)/boot/grub"    │
└─────────────────────────────┘
```

---

### 步骤 5：写入 boot.img 和 core.img 到 boot area

```makefile
@dd if=/usr/lib/grub/i386-pc/boot.img of=build/boot_area.img \
    bs=446 count=1 conv=notrunc status=none
@dd if=build/core.img of=build/boot_area.img \
    bs=512 seek=1 conv=notrunc status=none
```

**关键细节**：

| 写入内容 | 目标偏移 | 大小限制 | 说明 |
|---------|---------|---------|------|
| `boot.img` | 偏移 0，446 字节 | 精确 446B | 不覆盖分区表区（446~511） |
| `core.img` | 偏移 512（扇区 1）| 无限制 | 在 MBR gap 内，不触碰分区 |

`conv=notrunc` 确保 dd 不截断目标文件，只覆盖指定区域。

---

### 步骤 6：拼接 boot area + partition → 最终磁盘镜像

```makefile
@cat build/boot_area.img build/partition.img > build/LulaOS.img
```

**拼接结果**：
```
boot_area.img  (1MB)  +  partition.img  (63MB)  =  LulaOS.img  (64MB)
偏移 0x000000             偏移 0x100000              偏移 0x000000
LBA 0                     LBA 2048                   LBA 0 ~ 131071
```

分区起始位置 1MB = LBA 2048，与 MBR 分区表中的 `start=2048` 完全对齐。

---

### 步骤 7：写入 MBR 分区表

```makefile
@echo "start=2048, type=83, bootable" | sfdisk -q build/LulaOS.img
```

**sfdisk 写入内容（MBR 偏移 446~511）**：

```
偏移 446: 分区条目 1 (16 字节)
  [0]    0x80        可引导标志
  [1-3]  CHS 起始    0x00/0x01/0x00
  [4]    0x83        分区类型: Linux
  [5-7]  CHS 结束    饱和值 (1023/254/63)
  [8-11] 0x00000800  LBA 起始: 2048
  [12-15]0x0001F800  LBA 大小: 129024 扇区

偏移 462: 分区条目 2~4 (全零，未使用)
偏移 510: 0x55 0xAA  (引导签名)
```

`sfdisk` 只修改 446~511 字节区域，**不触碰前 446 字节**（boot.img 代码区），因此 boot.img 保持不变。

---

## 构建中间文件一览

| 文件 | 大小 | 用途 |
|------|------|------|
| `build/staging/boot/LulaOS.bin` | 内核 ELF | staging 目录，mke2fs 数据源 |
| `build/staging/boot/grub/grub.cfg` | ~100B | GRUB 菜单配置 |
| `build/boot_area.img` | 1MB | MBR + boot.img + core.img |
| `build/partition.img` | 63MB | ext2 文件系统（含内核和 grub.cfg）|
| `build/core.img` | ~25KB | GRUB 核心镜像（含内嵌模块） |
| `build/LulaOS.img` | 64MB | 最终磁盘镜像（cat 拼接后） |

---

## 各工具职责汇总

| 工具 | 来源包 | 职责 |
|------|-------|------|
| `dd` | coreutils | 创建全零文件、精确字节写入 |
| `mke2fs` | e2fsprogs | 创建 ext2 文件系统（`-d` 带内容）|
| `grub-mkimage` | grub2-common | 生成带内嵌模块和 prefix 的 core.img |
| `sfdisk` | util-linux | 脚本化写入 MBR 分区表 |
| `cat` | coreutils | 将 boot area 和 partition 拼接为完整磁盘 |

> 整个构建过程**不需要 root 权限**，不依赖 loop 设备，完全在用户态完成。
