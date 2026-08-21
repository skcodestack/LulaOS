#include <mm/slab.h>
#include <mm/mm.h>
#include <mm/mmzone.h>
#include <arch/x86/page.h>
#include <printk.h>
#include <libs/memcpy.h>
#include <stddef.h>

/*
 * ==================== 静态缓存描述符池 ====================
 *
 * 内核对象缓存描述符本身也占用内存。
 * 为避免“鸡生蛋”问题，此处预分配静态池，最多容纳 KMEM_CACHE_MAX 个缓存。
 */
static kmem_cache_t cache_pool[KMEM_CACHE_MAX];
static unsigned int cache_pool_used = 0;

/* 全局缓存链表：所有已创建的缓存均链在此处，供调试/统计遍历 */
static LIST_HEAD(cache_cache);

/* 自旋锁：保护 slab 链表操作（SMP 安全） */
static spinlock_t slab_lock = SPIN_LOCK_UNLOCKED;

/*
 * slab_initialized：kmem_cache_init() 完成后置 1
 *
 * 参考 Linux 2.6.20 slab_early_init：
 *   引导期间不允许 OFF_SLAB（slabp_cache 尚未创建），
 *   初始化完成后，大对象缓存可以启用 OFF_SLAB。
 */
static unsigned int slab_initialized = 0;

/*
 * ==================== 通用分配器缓存池 ====================
 *
 * 参考 Linux 2.6.20 mm/slab.c malloc_sizes[]
 * 支持 9 个固定尺寸：32/64/128/256/512/1024/2048/4096/8192 字节
 * 由 kmem_cache_init() 末尾创建，应用于 kmalloc/kfree
 */
kmem_cache_t *malloc_caches[9];

static const unsigned int malloc_sizes[9] = {
    32, 64, 128, 256, 512, 1024, 2048, 4096, 8192
};

#define MALLOC_CACHE_COUNT  9
#define MALLOC_MAX_SIZE     8192   /* kmalloc 支持的最大尺寸 */

/* ==================== 内部工具函数 ==================== */

/*
 * 字符串拷贝（限定最大长度，保证以 '\0' 结尾）
 */
static void slab_strncpy(char *dst, const char *src, unsigned int maxlen)
{
    unsigned int i;
    for (i = 0; i < maxlen - 1 && src[i] != '\0'; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/*
 * 计算每个 slab 页可容纳的对象数
 *
 * 内存布局:
 *   [slab_t 描述符 (sizeof(slab_t))] [对齐填充] [obj0][obj1]...[objN-1]
 *   总大小 = PAGE_SIZE << order
 *
 * 返回: 对象个数，0 表示对象过大无法放入
 */
static unsigned int calc_num_objs(unsigned int aligned_size, unsigned int order,
                                  unsigned int off_slab)
{
    unsigned int total = PAGE_SIZE << order;

    if (off_slab) {
        /* OFF_SLAB：描述符在页外，整页都用于对象 */
        if (aligned_size == 0)
            return 0;
        return total / aligned_size;
    }

    /* ON_SLAB：描述符嵌入页首 */
    unsigned int hdr_size    = sizeof(slab_t);
    unsigned int obj_start   = (hdr_size + SLAB_ALIGN_BYTES - 1)
                               & ~(SLAB_ALIGN_BYTES - 1);
    unsigned int usable      = total - obj_start;

    if (aligned_size == 0 || usable < aligned_size)
        return 0;

    return usable / aligned_size;
}

/*
 * 初始化一个 slab 页的空闲对象链表
 *
 * 每个空闲对象的前 sizeof(unsigned int) 字节存储下一个空闲对象的索引，
 * 最后一个对象存储 SLAB_NULL 表示链表结束。
 *
 * 链表形态（4 个对象示例）:
 *   obj0[→1] → obj1[→2] → obj2[→3] → obj3[→SLAB_NULL]
 */
static void slab_init_objs(slab_t *slab, unsigned int num, unsigned int aligned_size)
{
    for (unsigned int i = 0; i < num; i++) {
        unsigned int *next_ptr =
            (unsigned int *)((char *)slab->s_mem + i * aligned_size);
        if (i < num - 1)
            *next_ptr = i + 1;
        else
            *next_ptr = SLAB_NULL;
    }
}

/*
 * 单个 slab 占用的页面阶数
 * 从缓存描述符中读取预先计算的 alloc_order
 */
static inline unsigned int cache_order(kmem_cache_t *cache)
{
    return cache->alloc_order;
}

/*
 * 从伙伴系统申请页，构建 slab 描述符并挂入 empty 链表
 *
 * 返回: 新 slab 描述符指针，失败返回 NULL
 */
static slab_t *kmem_cache_grow(kmem_cache_t *cache)
{
    /* __alloc_pages(gfp_mask, order): 0 = ZONE_NORMAL zonelist */
    unsigned int order = cache_order(cache);
    struct page *page = __alloc_pages(0, order);
    if (!page) {
        printk("[SLAB] OOM: cannot grow cache '%s'\n", cache->name);
        return NULL;
    }

    /* 获取页的内核虚拟地址（直接映射区，由 zone_init 初始化） */
    void *vaddr = page->virtual;
    slab_t *slab;

    if (cache->off_slab && cache->slabp_cache) {
        /*
         * 【OFF_SLAB】描述符从 slabp_cache（kmalloc-32）独立分配
         *
         * 参考 Linux 2.6.20 alloc_slabmgmt()：
         *   if (OFF_SLAB(cachep))
         *       slabp = kmem_cache_alloc_node(cachep->slabp_cache, ...);
         *   slabp->s_mem = objp + colour_off;  // OFF_SLAB 时 colour_off=0
         *
         * slab 页 100% 用于对象：s_mem = vaddr（页基址），
         * 保证 8KB 对齐，使 current 宏（esp & ~0x1FFF）正确工作。
         */
        slab = (slab_t *)kmem_cache_alloc(cache->slabp_cache);
        if (!slab) {
            __free_pages(page, order);
            printk("[SLAB] OOM: cannot alloc off-slab descriptor for '%s'\n",
                   cache->name);
            return NULL;
        }
        slab->s_mem = (char *)vaddr;   /* 对象从页基址开始，完美对齐 */

        /* 在 slab 页末尾存储描述符指针（供 kmem_slab_destroy 反查） */
        unsigned int total = PAGE_SIZE << order;
        slab_t **backptr = (slab_t **)((char *)vaddr + total - sizeof(slab_t *));
        *backptr = slab;
    } else {
        /* 【ON_SLAB】描述符嵌入 slab 页首（原有逻辑） */
        slab = (slab_t *)vaddr;
        unsigned int obj_offset = (sizeof(slab_t) + SLAB_ALIGN_BYTES - 1)
                                  & ~(SLAB_ALIGN_BYTES - 1);
        slab->s_mem = (char *)vaddr + obj_offset;
    }

    slab->inuse = 0;
    slab->free  = 0;

    /* 初始化空闲对象嵌入式链表 */
    slab_init_objs(slab, cache->num, cache->aligned_size);

    /*
     * 在 page->virtual 存储 slab 描述符指针（用于 kmem_cache_free 反查）
     * OFF_SLAB 时：page->virtual 指向独立分配的描述符（不是 vaddr）
     * ON_SLAB 时：page->virtual = slab = vaddr（描述符在页首）
     */
    unsigned int n_pages = 1u << order;
    for (unsigned int i = 0; i < n_pages; i++) {
        page[i].virtual = (void *)slab;
        PageSetSlab(&page[i]);
    }

    /* 挂入缓存的 empty 链表 */
    list_add(&slab->list, &cache->empty);
    cache->num_slabs++;

    return slab;
}

/*
 * 销毁一个 slab 页：从缓存链表摘下，归还伙伴系统
 */
static void kmem_slab_destroy(kmem_cache_t *cache, slab_t *slab)
{
    /* 从链表摘下 */
    list_del(&slab->list);
    cache->num_slabs--;

    void *vaddr;

    if (cache->off_slab && cache->slabp_cache) {
        /*
         * 【OFF_SLAB】页基址从 s_mem 获得（s_mem = 页基址），
         * 描述符本身由 slabp_cache 回收。
         */
        vaddr = slab->s_mem;
    } else {
        /* 【ON_SLAB】描述符嵌入在页首，页基址 = slab 地址向下取整 */
        vaddr = (void *)((unsigned long)slab & PAGE_MASK);
    }

    struct page *page = virt_to_page(vaddr);
    unsigned int order = cache_order(cache);
    unsigned int n_pages = 1u << order;

    /* 还原两页的 virtual 指针，清除 slab 标志后归还伙伴系统 */
    for (unsigned int i = 0; i < n_pages; i++) {
        page[i].virtual = (void *)((unsigned long)vaddr + i * PAGE_SIZE);
        PageClearSlab(&page[i]);
    }
    __free_pages(page, order);

    if (cache->off_slab && cache->slabp_cache) {
        /* OFF_SLAB：描述符归还给 slabp_cache */
        kmem_cache_free(cache->slabp_cache, slab);
    }
}

/* ==================== 公共 API 实现 ==================== */

/*
 * kmem_cache_init - 初始化 slab 子系统
 *
 * 主要初始化静态数据结构（全局链表、静态池计数）。
 * 必须在 mm_init() 之后调用，以保证伙伴系统已就绪。
 * 在末尾创建通用大小缓存，供 kmalloc/kfree 使用。
 */
void kmem_cache_init(void)
{
    INIT_LIST_HEAD(&cache_cache);
    cache_pool_used = 0;
    slab_lock       = SPIN_LOCK_UNLOCKED;
    printk("slab: kmem_cache_init complete\n");

    /* 创建通用大小缓存（参考 Linux 2.6.20 malloc_sizes[]） */
    static const char *names[MALLOC_CACHE_COUNT] = {
        "kmalloc-32",   "kmalloc-64",   "kmalloc-128",
        "kmalloc-256",  "kmalloc-512",  "kmalloc-1024",
        "kmalloc-2048", "kmalloc-4096", "kmalloc-8192"
    };
    for (unsigned int i = 0; i < MALLOC_CACHE_COUNT; i++) {
        malloc_caches[i] = kmem_cache_create(names[i], malloc_sizes[i]);
        if (!malloc_caches[i])
            printk("[SLAB] WARNING: failed to create %s cache\n", names[i]);
    }

    /*
     * 引导完成，允许后续创建的缓存使用 OFF_SLAB
     *
     * 注意：此时 malloc_caches[] 已在引导期间创建完毕，
     * 引导时 slab_initialized=0，所有 malloc 缓存都是 ON_SLAB。
     * 置位后，此后创建的缓存（若 size>=512）将启用 OFF_SLAB，
     * slabp_cache 指向 kmalloc-32（足够容纳 sizeof(slab_t)=20B）。
     */
    slab_initialized = 1;

    /* 为所有已创建的大对象 malloc 缓存补设 slabp_cache */
    for (unsigned int i = 0; i < MALLOC_CACHE_COUNT; i++) {
        if (malloc_caches[i] && malloc_sizes[i] >= (PAGE_SIZE >> 3)) {
            malloc_caches[i]->off_slab    = 1;
            malloc_caches[i]->slabp_cache = malloc_caches[0]; /* kmalloc-32 */
            /*
             * OFF_SLAB 后描述符不占页内空间，重算 num：
             * ON_SLAB 时 kmalloc-8192 num=0（20字节头部装不下 8192 对象），
             * OFF_SLAB 时 num=1（整页 8192 字节全用于对象，恰好一个）。
             */
            unsigned int new_num = calc_num_objs(
                malloc_caches[i]->aligned_size,
                malloc_caches[i]->alloc_order, 1);
            malloc_caches[i]->num = new_num;
            printk("[SLAB] %s: enabled OFF_SLAB, num=%u (slabp from kmalloc-32)\n",
                   malloc_caches[i]->name, new_num);
        }
    }
}

/*
 * kmem_cache_create - 创建一个新的对象缓存
 *
 * 从静态描述符池分配 kmem_cache_t，计算对象布局参数，
 * 但不立即分配 slab 页（延迟到第一次 alloc 时触发 grow）。
 */
kmem_cache_t *kmem_cache_create(const char *name, unsigned int size)
{
    /* 静态池容量检查 */
    if (cache_pool_used >= KMEM_CACHE_MAX) {
        printk("[SLAB] cache pool full, cannot create '%s'\n",
               name ? name : "unnamed");
        return NULL;
    }

    /* 参数校验 */
    if (size == 0) {
        printk("[SLAB] kmem_cache_create: size must be > 0\n");
        return NULL;
    }

    /* 对象最小尺寸：须能容纳空闲链表索引 */
    if (size < SLAB_MIN_SIZE)
        size = SLAB_MIN_SIZE;

    /* 对齐到 SLAB_ALIGN_BYTES（4 字节） */
    unsigned int aligned_size = (size + SLAB_ALIGN_BYTES - 1)
                                & ~(SLAB_ALIGN_BYTES - 1);

    /*
     * 动态计算最小 order：slab_t 头占用页首若干字节，
     * 若 aligned_size 接近 PAGE_SIZE << order 则需提升 order。
     * 参考 Linux 2.6.20 kmem_cache_create：对象至少容许 1 个/slab。
     *
     * 【OFF_SLAB 判断】参考 Linux 2.6.20 kmem_cache_create 第 2281 行：
     *   if ((size >= (PAGE_SIZE >> 3)) && !slab_early_init)
     *       flags |= CFLGS_OFF_SLAB;
     *
     * 对象大小 >= PAGE_SIZE/8(512B) 且 slab 子系统已初始化时，
     * 启用 OFF_SLAB：描述符从 slabp_cache 独立分配，
     * slab 页 100% 用于对象，s_mem = 页基址（保证 8KB 对齐）。
     */
    unsigned int off_slab_flag = 0;
    if (size >= (PAGE_SIZE >> 3) && slab_initialized)
        off_slab_flag = 1;

    unsigned int order = 0;
    unsigned int num;
    while ((num = calc_num_objs(aligned_size, order, off_slab_flag)) == 0) {
        order++;
        if (order > 5) {  /* 最大 32 页 = 128KB，超出则拒绝创建 */
            printk("[SLAB] object too large for slab: %u bytes\n", size);
            return NULL;
        }
    }

    /* 从静态池取出一个描述符 */
    kmem_cache_t *cache = &cache_pool[cache_pool_used++];

    /* 填写描述符 */
    cache->obj_size     = size;
    cache->aligned_size = aligned_size;
    cache->alloc_order  = order;
    cache->num          = num;
    cache->num_slabs    = 0;
    cache->off_slab     = off_slab_flag;
    cache->slabp_cache  = NULL;   /* 在 kmem_cache_init 完成后设置 */

    INIT_LIST_HEAD(&cache->partial);
    INIT_LIST_HEAD(&cache->full);
    INIT_LIST_HEAD(&cache->empty);

    /* 设置名称 */
    if (name)
        slab_strncpy(cache->name, name, KMEM_CACHE_NAMELEN);
    else
        slab_strncpy(cache->name, "unnamed", KMEM_CACHE_NAMELEN);

    /* 链入全局缓存链表 */
    list_add_tail(&cache->list, &cache_cache);

    printk("[SLAB] created '%s': obj=%u aligned=%u num=%u/slab order=%u\n",
           cache->name, cache->obj_size, cache->aligned_size,
           cache->num, cache->alloc_order);

    return cache;
}

/*
 * kmem_cache_alloc - 从缓存中分配一个对象
 *
 * 查找顺序：partial 链表 → empty 链表 → kmem_cache_grow()
 * 分配后若 slab 变满，从 partial 移入 full。
 */
void *kmem_cache_alloc(kmem_cache_t *cache)
{
    unsigned long flags;
    void *obj = NULL;

retry:
    spin_lock_irqsave(&slab_lock, flags);

    /* ---- 优先从 partial 链表取（部分使用的 slab） ---- */
    slab_t *slab = NULL;

    if (!list_empty(&cache->partial)) {
        slab = list_entry(cache->partial.next, slab_t, list);
        goto got_slab;
    }

    /* ---- 其次从 empty 链表取（完全空闲的 slab） ---- */
    if (!list_empty(&cache->empty)) {
        slab = list_entry(cache->empty.next, slab_t, list);
        list_del(&slab->list);
        list_add(&slab->list, &cache->partial);
        goto got_slab;
    }

    /* ---- 均无，释放锁后向伙伴系统申请新 slab 页 ---- */
    spin_unlock_irqrestore(&slab_lock, flags);

    slab_t *new_slab = kmem_cache_grow(cache);
    if (!new_slab)
        return NULL;

    /*
     * 重新加锁后，不能直接使用 new_slab：
     * 在 unlock→lock 窗口期间，另一 CPU 可能已将 new_slab 消耗。
     * 因此重新从 partial/empty 链表取，保证指针有效。
     */
    goto retry;

got_slab:
    {
        /* 弹出空闲链表头节点 */
        unsigned int idx     = slab->free;
        unsigned int *next_p = (unsigned int *)((char *)slab->s_mem
                                                + idx * cache->aligned_size);
        slab->free = *next_p;   /* 推进空闲链表头 */
        slab->inuse++;

        obj = (char *)slab->s_mem + idx * cache->aligned_size;

        /* 若 slab 已满，从 partial 移入 full */
        if (slab->inuse == cache->num) {
            list_del(&slab->list);
            list_add(&slab->list, &cache->full);
        }
    }

    spin_unlock_irqrestore(&slab_lock, flags);
    return obj;
}

/*
 * kmem_cache_free - 将对象归还给所属缓存
 *
 * 反查路径: obj → virt_to_page → page->virtual → slab_t
 * 归还后若 slab 全空，从 empty 保留（可择机回收）；
 * 若从 full 变 partial，移入 partial 链表。
 */
void kmem_cache_free(kmem_cache_t *cache, void *obj)
{
    if (!obj)
        return;

    unsigned long flags;

    /* ---- 通过 page->virtual 反查 slab 描述符 ---- */
    struct page *page = virt_to_page(obj);
    slab_t *slab      = (slab_t *)page->virtual;

    /* 计算对象在 slab 内的索引 */
    unsigned int idx = ((unsigned long)obj - (unsigned long)slab->s_mem)
                       / cache->aligned_size;

    spin_lock_irqsave(&slab_lock, flags);

    /* 将对象推入空闲链表头部 */
    unsigned int *next_ptr = (unsigned int *)obj;
    *next_ptr  = slab->free;
    slab->free = idx;

    unsigned int old_inuse = slab->inuse;
    slab->inuse--;

    /* ---- 更新 slab 所在链表 ----
     *
     * 用 old_inuse（递减前的值）判断 slab 当前所在链表：
     *   old_inuse == num  → 在 full 链表，释放后移入 partial
     *   new_inuse == 0   → 在 partial 链表，释放后移入 empty
     *   其他             → 仍在 partial，无需移动
     */
    if (slab->inuse == 0) {
        /* partial → empty */
        list_del(&slab->list);
        list_add(&slab->list, &cache->empty);
    } else if (old_inuse == cache->num) {
        /* full → partial */
        list_del(&slab->list);
        list_add(&slab->list, &cache->partial);
    }

    spin_unlock_irqrestore(&slab_lock, flags);
}

/* ==================== 通用分配器 kmalloc / kfree ==================== */

/*
 * kmalloc - 通用内存分配
 *
 * 从 malloc_caches[] 中选择最小满足 size 的缓存，调用 kmem_cache_alloc 分配。
 * 对于 size == THREAD_SIZE(8192)：缓存使用 order=1 分配 2 页，
 *   返回地址为 8KB 对齐（伙伴系统 order=1 保证），满足 current 宏 esp屏蔽需求。
 *
 * 参数：
 *   size  - 需要分配的字节数（最大 MALLOC_MAX_SIZE）
 *   flags - GFP_KERNEL / GFP_ATOMIC（当前未区分，预留）
 * 返回：对齐的内存指针，失败返回 NULL
 */
void *kmalloc(unsigned int size, unsigned int flags)
{
    (void)flags;

    if (size == 0 || size > MALLOC_MAX_SIZE) {
        printk("[SLAB] kmalloc: invalid size %u\n", size);
        return NULL;
    }

    /* 查表找最小满足 size 的缓存 */
    for (unsigned int i = 0; i < MALLOC_CACHE_COUNT; i++) {
        if (size <= malloc_sizes[i]) {
            if (!malloc_caches[i]) {
                printk("[SLAB] kmalloc: cache[%u] not initialized\n", i);
                return NULL;
            }
            return kmem_cache_alloc(malloc_caches[i]);
        }
    }

    printk("[SLAB] kmalloc: no suitable cache for size %u\n", size);
    return NULL;
}

/*
 * kfree - 释放 kmalloc 分配的内存
 *
 * 通过 virt_to_page(obj)->virtual 反查 slab 描述符，
 * 再逐个扫描 malloc_caches[] 找到对应缓存调用 kmem_cache_free。
 *
 * 注：此处清楚对象属于哪个缓存需要扫描，
 *       若性能敏感可将 cache 指针嵌入对象头部优化。
 */
void kfree(const void *obj)
{
    if (!obj)
        return;

    struct page *page = virt_to_page(obj);
    if (!PageSlab(page)) {
        printk("[SLAB] kfree: %p is not a slab object\n", obj);
        return;
    }

    slab_t *slab = (slab_t *)page->virtual;

    /*
     * 找到对应的 malloc_caches[]
     * 通过对象指针和 slab->s_mem 内算索引，再由索引利用 aligned_size 反推对应缓存
     */
    for (unsigned int i = 0; i < MALLOC_CACHE_COUNT; i++) {
        kmem_cache_t *cache = malloc_caches[i];
        if (!cache)
            continue;
        /* 判断对象是否和该缓存匹配：对象指针在 slab 对象区内，且偏移整除 aligned_size */
        if (obj >= (void *)slab->s_mem) {
            unsigned int off = (unsigned long)obj - (unsigned long)slab->s_mem;
            if (off % cache->aligned_size == 0 &&
                off / cache->aligned_size < cache->num) {
                kmem_cache_free(cache, (void *)obj);
                return;
            }
        }
    }

    printk("[SLAB] kfree: %p not found in any malloc_cache\n", obj);
}
