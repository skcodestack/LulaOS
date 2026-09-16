# LulaOS 文件系统与 VFS 全链路实施计划

参考版本：Linux 2.6.20（与项目现有代码风格一致）。
磁盘侧已就绪：MBR 分区表（type 83, LBA 2048 起 63MB ext2，含 /boot/LulaOS.bin），无需 initrd。

## Task 1: 时间子系统（jiffies + RTC/CMOS + xtime）
- 新增 `includes/time.h` + `kernel/time.c`：全局 jiffies、xtime、mktime()（秒数→年月日，移植 Linux 2.6.20）
- 新增 `includes/arch/x86/rtc.h` + `arch/x86/kernel/rtc.c`：CMOS RTC 读取（BCD 转换）
- `kernel/interrupts/interrupts.c` 的 do_apic_timer_interrupt 中累加 jiffies 并 tick xtime
- 验证：启动时 printk 真实日期时间，jiffies 递增

## Task 2: 用户态内存访问接口
- 新增 `includes/arch/x86/uaccess.h`：copy_from_user/copy_to_user/strncpy_from_user
- 含地址合法性校验（用户指针必须 < PAGE_OFFSET）
- 改造 sys_write 先走 copy_from_user 作验证

## Task 3: 块设备核心（dev_t + gendisk + make_request）
- 新增 `includes/block/blkdev.h`：dev_t、MAJOR/MINOR/MKDEV、gendisk、block_device、block_device_operations
- 新增 `kernel/block/blk-core.c`：register_blkdev、块设备注册表、add_disk
- 简化 I/O 路径：submit_bh 同步直调驱动 strategy（暂不做请求队列与电梯调度）

## Task 4: buffer cache（buffer_head）
- 新增 `includes/block/buffer_head.h` + `kernel/block/buffer.c`
- buffer_head 结构 + 哈希表 + LRU：__getblk/bread/bwrite/brelse/mark_buffer_dirty/sync_buffers
- 验证：bread 读 LBA0 与现有 ata_read_mbr 输出一致

## Task 5: ATA/SATA 接入块层 + MBR 分区解析
- `kernel/ata/ata.c`：探测成功后 add_disk 注册 hd0（扇区读写函数作为 strategy 回调）
- 新增 `kernel/block/partition.c`：MBR 分区表解析（4 主分区）→ hd0p1（LBA 2048/size 129024）
- 验证：printk 分区表信息与 sfdisk 写入值一致

## Task 6: VFS 四大对象与缓存
- 新增 `includes/fs.h`：super_block/inode/dentry/file 四对象 + super_operations/inode_operations/file_operations 三张表 + file_system_type
- 新增 `kernel/fs/inode.c`（inode 缓存）、`kernel/fs/dcache.c`（dentry 哈希+LRU）、`kernel/fs/file_table.c`（file 结构池）
- 扩展 task_struct：增加 files/fs 字段（fork 时引用计数共享，等价 CLONE_FILES）

## Task 7: ramfs + rootfs 垫底
- 新增 `kernel/fs/ramfs.c`：内存文件系统（create/mkdir/mknod/symlink/写文件页）
- 内核启动时挂 rootfs（参考 init_mount_tree），/dev 挂点预留

## Task 8: 路径解析与 fd 表
- 新增 `kernel/fs/namei.c`：path_walk/open_namei/do_filp_open（含权限位简化）
- 新增 `kernel/fs/open.c`：getname/filp_open/filp_close/alloc_fd
- 新增 `kernel/fs/read_write.c`：vfs_read/vfs_write/vfs_lseek
- 验证：内核线程 open("/dev/console") 写入字符设备成功

## Task 9: 挂载机制 + 字符设备框架 + devtmpfs
- 新增 `kernel/fs/namespace.c`：vfsmount 树、do_mount、mount/umount2 系统调用
- 新增 `kernel/fs/char_dev.c`：register_chrdev、cdev 注册表、chrdev_open 按设备号分发 fops
- 新增 `kernel/fs/devices.c`：设备文件 inode（i_rdev、mknod）
- 新增 `kernel/fs/devtmpfs.c`：简化 devtmpfs（ramfs + 驱动注册自动创建 /dev 节点）
- TTY 接入 /dev/console（tty_fops）、fb 接入 /dev/fb0
- 验证：printk 重定向到 /dev/console 读写链路

## Task 10: ext2 只读路径
- 新增 `includes/ext2_fs.h`（磁盘布局：superblock/组描述符/inode/目录项常量）
- 新增 `kernel/ext2/super.c`：ext2_fill_super、get_sb、register_filesystem
- 新增 `kernel/ext2/inode.c`：ext2_iget、ext2_bmap（直接块+1/2/3 级间接）、readpage
- 新增 `kernel/ext2/namei.c`：ext2_lookup、ext2_readdir
- 验证：挂载 hd0p1，内核线程读取并打印 /boot/grub/grub.cfg 内容

## Task 11: ext2 完整读写路径
- 新增 `kernel/ext2/balloc.c`：块位图分配/释放（ext2_new_block/ext2_free_block）
- 新增 `kernel/ext2/ialloc.c`：inode 位图分配/释放
- 扩展 `kernel/ext2/namei.c`：create/mkdir/rmdir/unlink/目录项插入删除
- 扩展 `kernel/ext2/inode.c`：文件扩展分配、时间戳更新（依赖 Task 1）
- 验证：内核态创建/写入文件→重启→host 端 debugfs/e2fsck 校验文件系统一致性

## Task 12: 系统调用扩充
- 新增号位：open/close/read/write/lseek/ioctl/stat/fstat/dup2/mkdir/rmdir/getdents/chdir/getcwd/mknod/mount/umount2/gettimeofday/time
- sys_ioctl：经 fd 表路由到字符设备 fops->ioctl（打通 DRM ioctl 用户态通道）
- 改造 sys_write/sys_exit 占位实现为真实 VFS/进程语义
- 验证：全部编译通过，调用号表文档化

## Task 13: 用户态地址空间与 ELF 加载（execve）
- 扩展 `includes/mm/mm.h`：mm_struct、vm_area_struct；fork 复制地址空间（无 COW，整表复制）
- 新增 `kernel/fs/exec.c`：ELF32 头校验、PT_LOAD 段映射、bss 清零、用户栈构造（argv/envp）
- `kernel/sched.c`：copy_thread 支持用户进程（iret 返回用户态帧）
- `arch/x86/kernel/fault.c`：用户态缺页 → vma 查找 → 读盘映射（demand paging）
- execve 系统调用注册
- 验证：从 ext2 加载静态 ELF 用户程序并进入用户态执行

## Task 14: init 进程与用户程序构建链
- Makefile：新增 user/ 目录（i686 静态 ELF 编译），产物经 staging 进 mke2fs -d 镜像
- 新增 `user/init.c`（PID 1）+ `user/shell.c`（mini shell：fork+execve 跑内置命令 ls/cat/hello）
- 内核启动流程：挂载根文件系统 → run_init_process("/init")
- 验证：QEMU 启动 → 挂根 → 运行 /init → shell 交互（键盘输入、屏幕输出）

## Task 15: 集成回归与收尾
- 全量构建，Bochs + QEMU 双模拟器验证
- 回归：SMP 启动、DRM/fb 显示、USB、键盘鼠标不破坏
- 更新构建文档中的镜像内容说明（staging 加入 user 程序）

## 关键决策记录
- I/O 路径：同步 make_request，不做请求队列/电梯调度（后续可迭代）
- fd 表：fork 采用共享 files_struct（等价 CLONE_FILES），简化首个版本
- 无 COW：fork 整页表复制；缺页仅做 demand paging 文件映射
- /dev：简化 devtmpfs，驱动注册时自动创建节点
- 根文件系统：直接挂载磁盘 MBR 分区 1（ext2），不引入 initrd