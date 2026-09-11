/*
 * buffer_head.h - LulaOS 统一缓存：buffer_head 描述符
 *
 * 参考 Linux  的现代设计（page cache 与 buffer cache 合并）：
 *   include/linux/buffer_head.h
 *
 * 核心思想：
 *   - page cache 是唯一数据缓存（数据在 page->virtual 中）
 *   - buffer_head 降级为"块描述符"，挂在 page->buffers 环形链表上
 *   - 一页（4096B）挂 4 个 bh（blocksize=1024B），每个 bh 描述页内的一个逻辑块
 *   - bh->b_data 指向 page->virtual + offset，不额外分配数据缓冲区
 *
 * b_data 的有效性（kmap on-demand，参考 Linux fs/buffer.c）：
 *   ZONE_NORMAL 页：b_data = page->virtual + offset（直接映射，永久有效）
 *   ZONE_HIGHMEM 页：b_data = NULL！不长期占用 pkmap 槽位
 *     （pkmap 仅 1024 槽位 = 4MB，长期映射会导致缓存 > 4MB 时耗尽）
 *     需要 I/O 或访问数据时临时 kmap(bh->b_page)，用完立即 kunmap：
 *       void *kaddr = kmap(bh->b_page);
 *       ... 访问 kaddr + (b_blocknr % BLOCKS_PER_PAGE) * b_size ...
 *       kunmap(bh->b_page);
 *
 * 与 Linux 旧设计的对比：
 *   旧：buffer cache 独立哈希表，bh->b_data 指向独立 kmalloc 缓冲区
 *       → 同一磁盘块可能被 page cache 和 buffer cache 各缓存一份
 *   新：数据只在 page->virtual 中存一份，bh 仅是元数据描述符
 *       → 内存利用率更高，一致性自动保证
 */

#ifndef __BUFFER_HEAD_H__
#define __BUFFER_HEAD_H__

#include <mm/mm.h>            /* struct page, atomic_t, PageBuffers 等 */
#include <block/blkdev.h>     /* dev_t, struct block_device */

/* ======================== 块大小常量 ======================== */

/*
 * BLOCK_SIZE：ext2 默认逻辑块大小（字节）
 *
 * mkfs.ext2 对 64MB 分区默认使用 1024B 块（block_size = 1024）。
 * PAGE_SIZE / BLOCK_SIZE = 4096 / 1024 = 4，每页挂 4 个 buffer_head。
 *
 * ext2 superblock 中 s_log_block_size 字段记录块大小指数：
 *   blocksize = 1024 << s_log_block_size
 * 当 s_log_block_size = 0 → blocksize = 1024（默认）
 *
 * 后续 ext2 挂载时若发现块大小非 1024，需重新初始化 bh 参数。
 */
#define BLOCK_SIZE          1024
#define BLOCK_SIZE_BITS     10
#define BLOCKS_PER_PAGE     (PAGE_SIZE / BLOCK_SIZE)   /* 4 */

/* ======================== BH 状态标志（位标志） ======================== */

/*
 * BH 状态标志，存放在 buffer_head->b_state 中。
 * 参考 Linux  include/linux/buffer_head.h BH_* 枚举。
 *
 * BH_Uptodate：数据已从磁盘读入，内容有效（bread 成功后置位）。
 *               读请求前先检查此标志，命中则无需 I/O。
 *
 * BH_Dirty：    缓冲区数据被用户/内核修改，需写回磁盘（sync_buffers 时写回并清位）。
 *               同时设置对应 page 的 PG_dirty 标志（页级脏标志）。
 *
 * BH_Lock：     正在进行 I/O（LulaOS 单线程暂不使用，预留位，避免后续加锁改动结构体布局）。
 *
 * BH_Mapped：   bh 已映射到磁盘块，b_blocknr 有效（create_page_buffers 时置位）。
 */
enum bh_state_bits {
    BH_Uptodate = 0,    /* 数据有效（已从磁盘读入） */
    BH_Dirty    = 1,    /* 数据被修改，需写回 */
    BH_Lock     = 2,    /* I/O 进行中（预留） */
    BH_Mapped   = 3,    /* 已映射到磁盘块 */
};

/* 标志位操作宏（与 test_bit/set_bit 接口一致） */
#define buffer_uptodate(bh)   test_bit(BH_Uptodate, &(bh)->b_state)
#define set_buffer_uptodate(bh) set_bit(BH_Uptodate, &(bh)->b_state)

#define buffer_dirty(bh)      test_bit(BH_Dirty, &(bh)->b_state)
#define set_buffer_dirty(bh)  set_bit(BH_Dirty, &(bh)->b_state)
#define clear_buffer_dirty(bh) clear_bit(BH_Dirty, &(bh)->b_state)

#define buffer_mapped(bh)     test_bit(BH_Mapped, &(bh)->b_state)
#define set_buffer_mapped(bh) set_bit(BH_Mapped, &(bh)->b_state)

/* ======================== struct buffer_head ======================== */

/*
 * buffer_head - 统一缓存中的块描述符
 *
 * 每个逻辑块对应一个 bh，挂在其所属 page 的 buffers 环形链表上。
 *
 * 内存布局（32 位系统）：
 *   b_state(4) + b_this_page(4) + b_page(4) + b_blocknr(4) +
 *   b_size(4) + b_data(4) + b_bdev(4) + b_hash(8) + b_lru(8) + b_count(4)
 *   = 52 字节（kmalloc-64 槽，对齐到 64B）
 *
 * b_this_page：同页内 bh 的环形链表指针（bh0 → bh1 → bh2 → bh3 → bh0）
 *               遍历时从任一 bh 出发，绕一圈可访问同页全部 bh。
 *
 * b_hash：链入全局 bh 哈希表（按 bdev + b_blocknr 索引），
 *          __getblk 的 fast path 先查此表，命中则直接返回 bh。
 *
  * b_data：ZONE_NORMAL 页 = page->virtual + (b_blocknr % BLOCKS_PER_PAGE) * BLOCK_SIZE，
 *          永久有效，可直接访问。
 *          ZONE_HIGHMEM 页 = NULL（kmap on-demand 设计）：
 *          访问数据需临时 kmap(b_page) + 页内偏移，用完 kunmap 归还 pkmap 槽位。
 *          不额外分配内存，数据实际存放在 page 中。
 */
struct buffer_head {
    unsigned long        b_state;       /* BH_Uptodate / BH_Dirty / BH_Mapped 等 */
    struct buffer_head  *b_this_page;   /* 同页内环形链表（page->buffers 入口） */
    struct page         *b_page;        /* 所属 page（数据在此页的 virtual 中） */
    unsigned long        b_blocknr;     /* 逻辑块号（以 blocksize 为单位） */
    unsigned int         b_size;        /* 块大小（字节，固定 BLOCK_SIZE=1024） */
    char                *b_data;        /* normal页=virtual+offset；highmem页=NULL(kmap on-demand) */
    struct block_device *b_bdev;        /* 所属块设备 */
    struct list_head     b_hash;        /* 链入 bh 哈希表（按 bdev+blocknr 索引） */
    struct list_head     b_lru;         /* 链入 LRU 链表（b_count=0 时放入，回收时用） */
    atomic_t             b_count;       /* 引用计数（bread 增，brelse 减） */
};

/* ======================== 公共 API ======================== */

/*
 * buffer_cache_init - 初始化 buffer cache 子系统
 *
 * 清空 bh 哈希表（bh_hashtable[]）与 LRU 链表。
 * 必须在 block_dev_init() 之后、ata_init() 之前调用。
 */
void buffer_cache_init(void);

/*
 * __getblk - 查找或分配 buffer_head（核心入口）
 * @bdev:      目标块设备
 * @block:     逻辑块号（以 blocksize=1024 为单位，非扇区号）
 * @blocksize: 块大小（当前必须传 BLOCK_SIZE=1024，后续扩展支持变长）
 *
 * 流程（Linux  统一缓存路径）：
 *   1. 计算 page_index = block / BLOCKS_PER_PAGE
 *   2. bh 哈希表快速查找：命中 → b_count++，返回 bh
 *   3. 未命中：page_cache_lookup(page_index)
 *      - page 命中：找到 page 上对应 bh（b_blocknr == block），b_count++
 *      - page 未命中：__alloc_pages 分配新页，create_page_buffers 建 4 个 bh
 *   4. 返回 bh（b_count=1，数据可能尚未读入，需调用方检查 BH_Uptodate）
 *
 * 返回：buffer_head *，NULL 表示内存分配失败。
 */
struct buffer_head *__getblk(struct block_device *bdev,
                              unsigned long block, unsigned int blocksize);

/*
 * bread - 读块（__getblk + 触发 I/O）
 * @bdev:      目标块设备
 * @block:     逻辑块号（blocksize=1024 单位）
 * @blocksize: 块大小（固定 1024）
 *
 * 流程：
 *   1. bh = __getblk(bdev, block, blocksize)
 *   2. 若 BH_Uptodate 已置位 → 缓存命中，直接返回 bh（无 I/O）
 *   3. 未命中 → submit_bh(read) 将磁盘数据读入 bh->b_data
 *   4. 成功 → set_buffer_uptodate(bh)，返回 bh
 *   5. 失败 → brelse(bh)，返回 NULL
 *
 * 返回值含义（与 Linux 一致）：
 *   非 NULL：bh 有效，数据已在页中（normal 页 b_data 直接可读；
 *            highmem 页 b_data=NULL，访问前需临时 kmap(b_page)）
 *   NULL：I/O 失败或内存分配失败
 */
struct buffer_head *bread(struct block_device *bdev,
                           unsigned long block, unsigned int blocksize);

/*
 * bwrite - 写回单个 buffer_head 到磁盘
 * @bh: 目标 buffer_head（必须 BH_Mapped，b_bdev 有效）
 *
 * 流程：
 *   submit_bh(bh->b_bdev, sector=bh->b_blocknr*2, count=2, bh->b_data, write=1)
 *   成功 → clear_buffer_dirty(bh)，ClearPageDirty(bh->b_page)，返回 0
 *   失败 → 保持 BH_Dirty，返回 -EIO
 *
 * 返回：0 成功，负数失败。
 */
int bwrite(struct buffer_head *bh);

/*
 * brelse - 释放 buffer_head 引用
 * @bh: 要释放的 bh（可为 NULL，NULL 安全）
 *
 * 流程：
 *   atomic_dec(&bh->b_count)
 *   if b_count 归零 → list_add(bh->b_lru, lru_list)（放入 LRU，等待回收）
 *
 * 与 Linux 2.6 的区别：
 *   Linux 中 b_count=0 时触发 try_to_free_buffers（LRU shrinker），
 *   LulaOS 暂不实现回收，LRU 仅作记录（64MB 内存充足）。
 */
void brelse(struct buffer_head *bh);

/*
 * bforget - 强制丢弃 bh（不写回磁盘，用于错误路径）
 * @bh: 要丢弃的 bh（可为 NULL）
 *
 * 将 b_count 强制清零并从哈希表、LRU 移除，bh 所在页若所有 bh 均无引用
 * 则释放整页（后续 shrink_page_cache 时处理）。
 * 适用于 I/O 错误后放弃该块，避免脏数据被误写回。
 */
void bforget(struct buffer_head *bh);

/*
 * mark_buffer_dirty - 标记缓冲区为脏（数据已修改，需写回）
 * @bh: 目标 buffer_head
 *
 * 同时设置：
 *   - BH_Dirty 位（bh 级别）
 *   - PG_dirty 位（page 级别，供后续 page writeback 使用）
 */
void mark_buffer_dirty(struct buffer_head *bh);

/*
 * sync_buffers - 将指定设备上所有脏块写回磁盘
 * @bdev: 目标块设备（NULL 表示所有设备）
 *
 * 遍历 LRU 链表，将 BH_Dirty 且 b_bdev 匹配的 bh 通过 submit_bh(write) 写回。
 * 写回后清除 BH_Dirty 与 PG_dirty。
 *
 * 返回值：写回的块数（调试用）。
 */
int sync_buffers(struct block_device *bdev);

/*
 * bh_blocknr_to_sector - 逻辑块号转 512B 扇区号（辅助函数）
 * @block: 逻辑块号（blocksize=1024 单位）
 * 返回：512B 扇区号（block * 2）
 */
static inline unsigned long bh_blocknr_to_sector(unsigned long block)
{
    /* 1024B / 512B = 2 扇区/块 */
    return block * (BLOCK_SIZE / 512);
}

#endif /* __BUFFER_HEAD_H__ */
