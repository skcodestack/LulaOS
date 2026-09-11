/*
 * buffer.c - LulaOS 统一缓存：page cache + buffer cache 合并实现
 *
 * 参考 Linux  （page cache 与 buffer cache 合并后）：
 *   fs/buffer.c              — __getblk, bread, brelse, mark_buffer_dirty
 *   mm/filemap.c             — page cache 哈希表（find_get_page）
 *   include/linux/buffer_head.h — buffer_head 结构
 *
 * 架构（现代设计，对照 Linux  旧设计的改进）：
 *
 *   [page_hashtable] 按 (dev, page_index) 索引页
 *       └─ page->virtual[0..4095]   ← 数据只存这一份
 *           ├─ bh0 (block 0)  b_data = virtual + 0
 *           ├─ bh1 (block 1)  b_data = virtual + 1024
 *           ├─ bh2 (block 2)  b_data = virtual + 2048
 *           └─ bh3 (block 3)  b_data = virtual + 3072
 *               ↑ b_this_page 环形链表串联
 *
 *   [bh_hashtable] 按 (dev, blocknr) 索引 bh → __getblk 快速命中路径
 *   [lru_list]     b_count=0 的 bh 链入此处，等待后续回收（暂未实现回收）
 *
 * 关键流程：
 *   __getblk(bdev, block, size)
 *     → bh_hashtable 命中 → 直接返回
 *     → page_hashtable 命中 → 找到 page 上对应 bh
 *     → 均未命中 → __alloc_pages + create_page_buffers
 *
 *   bread(bdev, block, size)
 *     → __getblk → 若 !BH_Uptodate → submit_bh(read) → set_buffer_uptodate
 */

#include <block/buffer_head.h>
#include <block/blkdev.h>
#include <mm/mm.h>
#include <mm/mmzone.h>     /* __alloc_pages, __free_page */
#include <arch/x86/highmem.h>  /* kmap：ZONE_HIGHMEM 页建立虚拟地址映射 */
#include <mm/slab.h>       /* kmalloc, kfree, GFP_KERNEL */
#include <libs/memcpy.h>   /* memset */
#include <libs/string.h>   /* _memset */
#include <printk.h>

/* ======================== 哈希表常量 ======================== */

/*
 * page cache 哈希表：1024 槽（8KB .bss）
 * bh 哈希表：2048 槽（16KB .bss，bh 数量远多于页，哈希冲突更少）
 */
#define PAGE_HASH_BITS   10
#define PAGE_HASH_SIZE   (1UL << PAGE_HASH_BITS)   /* 1024 */
#define PAGE_HASH_MASK   (PAGE_HASH_SIZE - 1)

#define BH_HASH_BITS     11
#define BH_HASH_SIZE     (1UL << BH_HASH_BITS)     /* 2048 */
#define BH_HASH_MASK     (BH_HASH_SIZE - 1)

/* ======================== 静态全局数据 ======================== */

/* page cache 哈希表：槽内以 page->cache_list 串联 */
static struct list_head page_hashtable[PAGE_HASH_SIZE];

/* bh 哈希表：槽内以 bh->b_hash 串联，__getblk 快速路径命中 */
static struct list_head bh_hashtable[BH_HASH_SIZE];

/* LRU 链表：b_count=0 的 bh 链入此处（暂未实现回收，仅作记录） */
static LIST_HEAD(lru_list);

/* 统计信息（调试用） */
static unsigned long stat_page_alloc  = 0;  /* 已分配的 page cache 页数 */
static unsigned long stat_bh_alloc    = 0;  /* 已分配的 buffer_head 数 */
static unsigned long stat_bh_hit      = 0;  /* bh 哈希表命中次数 */
static unsigned long stat_bh_miss     = 0;  /* bh 哈希表未命中次数 */

/* ======================== 哈希函数 ======================== */

/*
 * page_hash - page cache 哈希函数
 *
 * 输入：(dev, page_index)，输出：[0, PAGE_HASH_SIZE) 槽号
 * 使用 XOR 散列（参考 Linux   page_hash in mm/filemap.c）
 */
static inline unsigned long page_hash_key(unsigned long dev, unsigned long index)
{
    return (dev ^ index) & PAGE_HASH_MASK;
}

/*
 * bh_hash - bh 哈希函数
 *
 * blocknr 乘以常数 2654435761（Knuth 黄金比例散列）后与 dev XOR，
 * 避免连续块号映射到相邻槽位，减少聚集冲突。
 */
static inline unsigned long bh_hash_key(unsigned long dev, unsigned long blocknr)
{
    unsigned long h = blocknr * 2654435761UL;
    return (dev ^ h) & BH_HASH_MASK;
}

/* ======================== page cache 操作 ======================== */

/*
 * page_cache_lookup - 查找已缓存的页
 * @dev:   块设备（dev_t 值，作为哈希键一部分）
 * @index: 页索引（block / BLOCKS_PER_PAGE）
 *
 * 遍历对应哈希槽，匹配 mapping_dev + index。
 * 命中时增加 page 引用计数（get_page），调用方用完后需 put_page。
 * 返回：struct page *，未命中返回 NULL。
 */
static struct page *page_cache_lookup(unsigned long dev, unsigned long index)
{
    unsigned long slot = page_hash_key(dev, index);
    struct list_head *head = &page_hashtable[slot];
    struct page *p;

    list_for_each_entry(p, head, cache_list) {
        if (p->mapping_dev == dev && p->index == index) {
            /* 命中：增加引用计数（防止释放） */
            get_page(p);
            return p;
        }
    }
    return NULL;
}

/*
 * page_cache_add - 将页加入 page cache 哈希表
 * @page: 已初始化好 mapping_dev 和 index 的页
 */
static void page_cache_add(struct page *page)
{
    unsigned long slot = page_hash_key(page->mapping_dev, page->index);
    list_add(&page->cache_list, &page_hashtable[slot]);
    stat_page_alloc++;
}

/*
 * page_cache_remove - 将页从 page cache 哈希表移除
 * @page: 要移除的页
 */
static void page_cache_remove(struct page *page)
{
    list_del(&page->cache_list);
    INIT_LIST_HEAD(&page->cache_list);
    stat_page_alloc--;
}

/* ======================== buffer_head 操作 ======================== */

/*
 * bh_add_hash - 将 bh 加入 bh 哈希表
 */
static void bh_add_hash(struct buffer_head *bh)
{
    unsigned long slot = bh_hash_key((unsigned long)bh->b_bdev->bd_dev,
                                     bh->b_blocknr);
    list_add(&bh->b_hash, &bh_hashtable[slot]);
    stat_bh_alloc++;
}

/*
 * bh_remove_hash - 将 bh 从 bh 哈希表移除
 */
static void bh_remove_hash(struct buffer_head *bh)
{
    list_del(&bh->b_hash);
    INIT_LIST_HEAD(&bh->b_hash);
    stat_bh_alloc--;
}

/*
 * bh_lookup - 在 bh 哈希表中查找指定 (bdev, blocknr) 的 bh
 *
 * 命中时增加 b_count 并返回 bh；未命中返回 NULL。
 * 这是 __getblk 的快速路径（fast path）。
 */
static struct buffer_head *bh_lookup(struct block_device *bdev,
                                      unsigned long blocknr)
{
    unsigned long slot = bh_hash_key((unsigned long)bdev->bd_dev, blocknr);
    struct list_head *head = &bh_hashtable[slot];
    struct buffer_head *bh;

    list_for_each_entry(bh, head, b_hash) {
        if (bh->b_bdev == bdev && bh->b_blocknr == blocknr) {
            atomic_inc(&bh->b_count);
            stat_bh_hit++;
            return bh;
        }
    }
    stat_bh_miss++;
    return NULL;
}

/*
 * page_find_bh - 在页的 b_this_page 环形链表中找到指定块号的 bh
 * @page:  目标页（page->buffers 为环形链表入口）
 * @block: 目标逻辑块号
 *
 * 返回：匹配的 buffer_head *，未找到返回 NULL
 */
static struct buffer_head *page_find_bh(struct page *page, unsigned long block)
{
    struct buffer_head *bh = page->buffers;
    if (!bh)
        return NULL;
    struct buffer_head *head = bh;
    do {
        if (bh->b_blocknr == block)
            return bh;
        bh = bh->b_this_page;
    } while (bh != head);
    return NULL;
}

/*
 * create_page_buffers - 为一页分配并初始化 BLOCKS_PER_PAGE 个 buffer_head
 * @page:  目标页（ZONE_NORMAL 页 page->virtual 有效；ZONE_HIGHMEM 页为 NULL）
 * @bdev:  块设备
 * @blocksize: 块大小（当前固定 1024）
 *
 * 分配 4 个 buffer_head（kmalloc-64），建立环形 b_this_page 链表。
 * 所有 bh 加入 bh_hashtable。
 *
 * b_data 的设置（kmap on-demand，参考 Linux fs/buffer.c）：
 *   ZONE_NORMAL 页：b_data = page->virtual + i*blocksize（直接映射，永久有效）
 *   ZONE_HIGHMEM 页：b_data = NULL（不在此处 kmap！）
 *     访问方（bread/bwrite/ext2）临时 kmap(bh->b_page) 后
 *     用 kaddr + (b_blocknr % BLOCKS_PER_PAGE)*blocksize 计算数据地址，
 *     用完立即 kunmap 归还 pkmap 槽位。
 *     这样 1024 个 pkmap 槽位可服务无限多 highmem 缓存页。
 *
 * 新创建的 bh 均不置 BH_Uptodate（数据尚未从磁盘读入）。
 */
static void create_page_buffers(struct page *page, struct block_device *bdev,
                                 unsigned int blocksize)
{
    unsigned long first_block = page->index * BLOCKS_PER_PAGE;
    struct buffer_head *head = NULL;
    struct buffer_head *prev = NULL;

    for (int i = 0; i < BLOCKS_PER_PAGE; i++) {
        struct buffer_head *bh = (struct buffer_head *)
            kmalloc(sizeof(struct buffer_head), GFP_KERNEL);
        if (!bh) {
            printk("buffer: create_page_buffers: kmalloc failed at i=%d\n", i);
            /* 已分配的 bh 无法回收（简化处理），直接 panic 路径 */
            return;
        }
        memset(bh, 0, sizeof(struct buffer_head));

        bh->b_state    = 0;
        bh->b_page     = page;
        bh->b_blocknr  = first_block + i;
        bh->b_size     = blocksize;
        /*
         * highmem 页 b_data = NULL（on-demand kmap 设计）：
         * 数据地址在 I/O / 访问时由 kmap(b_page) + 块内偏移临时计算。
         * normal 页 b_data = 直接映射地址 + 偏移，永久有效。
         */
        if (PageHighMem(page))
            bh->b_data = NULL;
        else
            bh->b_data = (char *)page->virtual + (unsigned long)i * blocksize;
        bh->b_bdev     = bdev;
        atomic_set(&bh->b_count, 0);
        INIT_LIST_HEAD(&bh->b_hash);
        INIT_LIST_HEAD(&bh->b_lru);
        set_buffer_mapped(bh);   /* b_blocknr 有效 */

        /* 建立环形链表 */
        if (!head) {
            head = bh;
        } else {
            prev->b_this_page = bh;
        }
        prev = bh;
    }

    /* 闭环：最后一个 bh 指向第一个 */
    prev->b_this_page = head;

    page->buffers = head;
    SetPageBuffers(page);

    /* 将所有 bh 加入 bh 哈希表 */
    struct buffer_head *curr = head;
    do {
        bh_add_hash(curr);
        curr = curr->b_this_page;
    } while (curr != head);
}

/* ======================== buffer_cache_init ======================== */

/*
 * buffer_cache_init - 初始化 buffer cache 子系统
 *
 * 清空 page_hashtable[]、bh_hashtable[]、lru_list。
 * 必须在 block_dev_init() 之后、ata_init() 之前调用。
 */
void buffer_cache_init(void)
{
    unsigned long i;
    for (i = 0; i < PAGE_HASH_SIZE; i++)
        INIT_LIST_HEAD(&page_hashtable[i]);
    for (i = 0; i < BH_HASH_SIZE; i++)
        INIT_LIST_HEAD(&bh_hashtable[i]);
    INIT_LIST_HEAD(&lru_list);

    stat_page_alloc = stat_bh_alloc = stat_bh_hit = stat_bh_miss = 0;
    printk("buffer: buffer cache initialized  (page_hash=%lu  bh_hash=%lu)\n",
           PAGE_HASH_SIZE, BH_HASH_SIZE);
}

/* ======================== __getblk ======================== */

/*
 * __getblk - 查找或分配 buffer_head（核心入口）
 *
 * 三层查找路径（参考 Linux  fs/buffer.c __getblk）：
 *
 *   [fast path] bh_hashtable 命中
 *     直接返回 bh（b_count++），无 I/O，O(1)
 *
 *   [slow path 1] page_hashtable 命中
 *     页已在 page cache 中（可能由相邻块的 bread 带入），
 *     找到该页上对应 blocknr 的 bh，b_count++，返回
 *
 *   [slow path 2] 均未命中
 *     __alloc_pages 分配新页 → create_page_buffers 建 4 个 bh
 *     加入 page_hashtable + bh_hashtable，返回目标 bh
 *
 * 注意：返回的 bh 数据不一定有效（BH_Uptodate 可能未置位），
 *       调用方（bread）需检查并触发读盘。
 */
struct buffer_head *__getblk(struct block_device *bdev,
                              unsigned long block, unsigned int blocksize)
{
    if (!bdev) {
        printk("__getblk: NULL bdev\n");
        return NULL;
    }

    /* ---- fast path：bh 哈希表 ---- */
    struct buffer_head *bh = bh_lookup(bdev, block);
    if (bh)
        return bh;

    /* ---- slow path 1：page cache 命中 ---- */
    unsigned long page_index = block / BLOCKS_PER_PAGE;
    struct page *page = page_cache_lookup(bdev->bd_dev, page_index);

    if (page) {
        /* 页已在 cache 中，找页内对应 bh */
        bh = page_find_bh(page, block);
        if (bh) {
            atomic_inc(&bh->b_count);
            put_page(page);   /* page_cache_lookup 增加的引用，bh 持有自己的引用 */
            return bh;
        }
        /* 理论上不应走到此处（页在但 bh 不在，说明数据结构损坏） */
        printk("__getblk: page found but bh missing! block=%lu\n", block);
        put_page(page);
    }

    /* ---- slow path 2：分配新页 ---- */
    page = __alloc_pages(0, 0);   /* gfp_mask=0 → ZONE_NORMAL，order=0 → 单页 */
    if (!page) {
        printk("__getblk: __alloc_pages failed for block=%lu\n", block);
        return NULL;
    }

    /*
     * kmap on-demand 设计（参考 Linux fs/buffer.c create_page_buffers）：
     *
     * 此处不调用 kmap！highmem 页的映射延迟到真正需要时：
     *   - I/O 路径（bread/bwrite）：临时 kmap → submit_bh → kunmap
     *     （I/O 完成即释放槽位，pkmap 1024 槽位可服务无限多缓存页）
     *   - 数据访问路径（未来 ext2）：kmap(bh->b_page) → 读数据 → kunmap
     *
     * ZONE_NORMAL 页：page->virtual 已在 zone_init 中由 __va(phys) 设置，
     *                create_page_buffers 直接建立永久有效的 b_data。
     * ZONE_HIGHMEM 页：page->virtual 保持 NULL，b_data = NULL，
     *                 避免 pkmap 被长期缓存页占满（32MB 缓存 >> 4MB pkmap）。
     */

    /* 初始化 page cache 相关字段 */
    page->mapping_dev = bdev->bd_dev;
    page->index       = page_index;
    page->buffers     = NULL;
    INIT_LIST_HEAD(&page->cache_list);

    /* 为整页创建 BLOCKS_PER_PAGE 个 buffer_head */
    create_page_buffers(page, bdev, blocksize);

    /* 加入 page cache 哈希表 */
    page_cache_add(page);

    /* 找到目标 bh（此时 b_count=0，需手动增引用） */
    bh = page_find_bh(page, block);
    if (!bh) {
        printk("__getblk: FATAL - bh not found after create_page_buffers\n");
        return NULL;
    }
    atomic_inc(&bh->b_count);

    return bh;
}

/* ======================== bread ======================== */

/*
 * bread - 读块（__getblk + 触发磁盘 I/O）
 *
 * 流程（参考 Linux  fs/buffer.c bread）：
 *   1. __getblk 获取 bh（可能来自 cache，可能是新分配的空 bh）
 *   2. 检查 BH_Uptodate：已置位 → 缓存命中，无 I/O，直接返回
 *   3. 未命中 → submit_bh(read)：
 *      sector = block * (1024/512) = block * 2
 *      count  = 1024 / 512 = 2 个扇区
 *      buf    = bh 数据区地址（normal 页直接用 b_data；
 *               highmem 页临时 kmap 计算，I/O 后立即释放）
 *   4. I/O 成功 → set_buffer_uptodate(bh)，返回 bh
 *   5. I/O 失败 → brelse(bh)，返回 NULL
 *
 * highmem 页的 I/O（kmap on-demand）：
 *   kmap(page) → kaddr + 页内偏移 → submit_bh → kunmap(page)
 *   同步 I/O 完成时数据已写入物理页，kmap 槽位可立即归还，
 *   后续访问数据由调用方再次临时 kmap。
 */
struct buffer_head *bread(struct block_device *bdev,
                           unsigned long block, unsigned int blocksize)
{
    struct buffer_head *bh = __getblk(bdev, block, blocksize);
    if (!bh)
        return NULL;

    /* 缓存命中：数据已在页中，无需 I/O */
    if (buffer_uptodate(bh))
        return bh;

    /* 触发磁盘读：1024B = 2 个 512B 扇区 */
    unsigned long sector = block * (BLOCK_SIZE / 512);
    unsigned int  count  = BLOCK_SIZE / 512;   /* 2 扇区 */

    /*
     * 计算 I/O 缓冲区地址：
     *   ZONE_NORMAL 页：bh->b_data 永久有效，直接使用
     *   ZONE_HIGHMEM 页：b_data 为 NULL，临时 kmap 占用 pkmap 槽位，
     *                    数据地址 = kaddr + (blocknr - 页首块号) * blocksize
     */
    char *io_buf = bh->b_data;
    int   did_kmap = 0;

    if (!io_buf) {
        void *kaddr = kmap(bh->b_page);
        if (!kaddr) {
            printk("bread: kmap failed for block=%lu (pkmap exhausted)\n", block);
            brelse(bh);
            return NULL;
        }
        unsigned long first_block = bh->b_page->index * BLOCKS_PER_PAGE;
        unsigned long offset = (bh->b_blocknr - first_block) * bh->b_size;
        io_buf   = (char *)kaddr + offset;
        did_kmap = 1;
    }

    int err = submit_bh(bdev, sector, count, io_buf, 0 /* read */);

    /* 同步 I/O 已完成，highmem 页立即归还 pkmap 槽位（数据已在物理页中） */
    if (did_kmap)
        kunmap(bh->b_page);

    if (err) {
        printk("bread: I/O error block=%lu err=%d\n", block, err);
        brelse(bh);
        return NULL;
    }

    /* I/O 成功：标记数据有效 */
    set_buffer_uptodate(bh);
    return bh;
}

/* ======================== bwrite ======================== */

/*
 * bwrite - 将单个 buffer_head 数据写回磁盘
 *
 * 通过 submit_bh(write) 同步写入。
 * 成功后清除 BH_Dirty 和页级 PG_dirty 标志。
 *
 * highmem 页的 I/O（kmap on-demand，与 bread 对称）：
 *   kmap(page) → kaddr + 页内偏移 → submit_bh → kunmap(page)
 *   同步写盘完成后槽位立即归还。
 *   写入前数据已由调用方修改完毕（mark_buffer_dirty 时的修改过程
 *   由调用方自行临时 kmap 完成）。
 */
int bwrite(struct buffer_head *bh)
{
    if (!bh || !bh->b_bdev) {
        printk("bwrite: NULL bh or bdev\n");
        return -1;
    }

    unsigned long sector = bh->b_blocknr * (BLOCK_SIZE / 512);
    unsigned int  count  = BLOCK_SIZE / 512;

    /*
     * 计算 I/O 缓冲区地址：
     *   ZONE_NORMAL 页：bh->b_data 永久有效，直接使用
     *   ZONE_HIGHMEM 页：b_data 为 NULL，临时 kmap 占用 pkmap 槽位
     */
    char *io_buf = bh->b_data;
    int   did_kmap = 0;

    if (!io_buf) {
        void *kaddr = kmap(bh->b_page);
        if (!kaddr) {
            printk("bwrite: kmap failed for block=%lu (pkmap exhausted)\n",
                   bh->b_blocknr);
            return -1;
        }
        unsigned long first_block = bh->b_page->index * BLOCKS_PER_PAGE;
        unsigned long offset = (bh->b_blocknr - first_block) * bh->b_size;
        io_buf   = (char *)kaddr + offset;
        did_kmap = 1;
    }

    int err = submit_bh(bh->b_bdev, sector, count, io_buf, 1 /* write */);

    /* 同步 I/O 已完成，highmem 页立即归还 pkmap 槽位 */
    if (did_kmap)
        kunmap(bh->b_page);

    if (err) {
        printk("bwrite: I/O error block=%lu err=%d\n", bh->b_blocknr, err);
        return err;
    }

    /* 写回成功：清除脏标志 */
    clear_buffer_dirty(bh);
    if (bh->b_page)
        ClearPageDirty(bh->b_page);
    return 0;
}

/* ======================== brelse ======================== */

/*
 * brelse - 释放 buffer_head 引用（Buffer RELeaSE）
 *
 * 减少 b_count；计数归零时将 bh 放入 LRU 链表（等待后续回收）。
 * NULL 安全（与 Linux 行为一致）。
 *
 * 参考 Linux 2.6.20 fs/buffer.c __brelse：
 *   if (atomic_read(&bh->b_count))
 *       __bforget(bh);
 *   else
 *       lru_add(bh);
 */
void brelse(struct buffer_head *bh)
{
    if (!bh)
        return;

    if (atomic_read(&bh->b_count) <= 0) {
        printk("brelse: b_count already zero for block=%lu!\n", bh->b_blocknr);
        return;
    }

    if (atomic_dec_and_test(&bh->b_count)) {
        /* 引用计数归零：放入 LRU（暂不实现回收） */
        list_add(&bh->b_lru, &lru_list);
    }
}

/* ======================== bforget ======================== */

/*
 * bforget - 强制丢弃 buffer_head（不写回磁盘，用于错误路径）
 *
 * 与 brelse 的区别：
 *   brelse：引用计数减一，归零时入 LRU（脏数据可能被 sync_buffers 写回）
 *   bforget：强制清除脏标志，从哈希表移除，即使 b_count>0 也丢弃
 *             确保脏数据不会被误写回（用于 I/O 错误后放弃该块）
 *
 * 参考 Linux 2.6.20 fs/buffer.c __bforget
 */
void bforget(struct buffer_head *bh)
{
    if (!bh)
        return;

    /* 清除脏标志（防止误写回） */
    clear_buffer_dirty(bh);

    /* 从 LRU 移除（若在其中） */
    if (!list_empty(&bh->b_lru)) {
        list_del(&bh->b_lru);
        INIT_LIST_HEAD(&bh->b_lru);
    }

    /* 从 bh 哈希表移除 */
    bh_remove_hash(bh);

    /* 强制清零引用计数 */
    atomic_set(&bh->b_count, 0);
}

/* ======================== mark_buffer_dirty ======================== */

/*
 * mark_buffer_dirty - 标记缓冲区为脏
 *
 * 同时设置：
 *   - BH_Dirty（bh 级别，sync_buffers 据此判断是否需要写回）
 *   - PG_dirty（page 级别，供后续 page writeback 扫描使用）
 */
void mark_buffer_dirty(struct buffer_head *bh)
{
    if (!bh)
        return;
    set_buffer_dirty(bh);
    if (bh->b_page)
        SetPageDirty(bh->b_page);
}

/* ======================== sync_buffers ======================== */

/*
 * sync_buffers - 将指定设备上所有脏块写回磁盘
 * @bdev: 目标设备（NULL 表示所有设备）
 *
 * 遍历 bh_hashtable 所有槽位，将 BH_Dirty 且 b_bdev 匹配的 bh 写回。
 * （不遍历 LRU：bh 在 LRU 中意味着 b_count=0，但脏标志仍有效）
 *
 * 返回：写回的块数（调试用）。
 */
int sync_buffers(struct block_device *bdev)
{
    unsigned long i;
    int written = 0;

    for (i = 0; i < BH_HASH_SIZE; i++) {
        struct list_head *head = &bh_hashtable[i];
        struct buffer_head *bh;

        list_for_each_entry(bh, head, b_hash) {
            /* 跳过非脏或非目标设备的 bh */
            if (!buffer_dirty(bh))
                continue;
            if (bdev && bh->b_bdev != bdev)
                continue;

            int err = bwrite(bh);
            if (err) {
                printk("sync_buffers: write failed block=%lu err=%d\n",
                       bh->b_blocknr, err);
            } else {
                written++;
            }
        }
    }

    if (written > 0)
        printk("sync_buffers: %d dirty buffers flushed\n", written);
    return written;
}
