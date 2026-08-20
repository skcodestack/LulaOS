#ifndef __SLAB_H__
#define __SLAB_H__

#include <libs/list.h>

/* ======================== 常量定义 ======================== */

/* 每个 slab 缓存描述符静态池大小（内核启动缓存数上限） */
#define KMEM_CACHE_MAX      32

/* 缓存名称最大长度 */
#define KMEM_CACHE_NAMELEN  32

/* 空闲对象链表终止符 */
#define SLAB_NULL           0xFFFFFFFFU

/* 对象最小尺寸：须能容纳一个 unsigned int 索引（空闲链表节点） */
#define SLAB_MIN_SIZE       (sizeof(unsigned int))

/* 对象默认对齐：4 字节（32 位系统 long 对齐） */
#define SLAB_ALIGN_BYTES    4

/* ======================== 数据结构 ======================== */

/*
 * slab_t - 单个 slab 页的描述符
 *
 * 嵌入在 slab 页的起始位置（page->virtual 指向此处）
 * 对象区紧随其后，从 s_mem 开始
 *
 * 页内存布局:
 *   [slab_t 描述符] [对齐填充] [obj0][obj1]...[objN-1]
 *                   ↑ s_mem
 */
typedef struct slab_s {
    struct list_head list;      /* 链入 partial / full / empty 链表 */
    void            *s_mem;    /* 第一个对象的起始虚拟地址 */
    unsigned int     inuse;    /* 已分配对象计数 */
    unsigned int     free;     /* 空闲链表头：下一个空闲对象索引，SLAB_NULL 表示满 */
} slab_t;

/*
 * kmem_cache_t - 对象缓存（slab 分配器的核心结构）
 *
 * 管理一组相同大小对象的分配/释放
 * 三条颜色链表：partial（部分使用）/ full（全满）/ empty（全空，可回收）
 */
struct kmem_cache {
    /* ---- 对象规格 ---- */
    unsigned int    obj_size;       /* 用户请求的原始对象大小 */
    unsigned int    aligned_size;   /* 实际存储大小（对齐后） */
    unsigned int    num;            /* 每个 slab 页容纳的对象数 */

    /* ---- slab 链表 ---- */
    struct list_head partial;       /* 有空闲对象的 slab */
    struct list_head full;          /* 所有对象均已分配的 slab */
    struct list_head empty;         /* 完全空闲的 slab（可回收） */

    /* ---- 统计 ---- */
    unsigned int    num_slabs;      /* 当前已分配的 slab 页数 */

    /* ---- 全局缓存链 ---- */
    struct list_head list;          /* 链入全局 cache_cache */

    /* ---- 名称 ---- */
    char            name[KMEM_CACHE_NAMELEN];
};

typedef struct kmem_cache kmem_cache_t;

/* ======================== GFP 分配标志 ======================== */

#define GFP_KERNEL  0x0001  /* 内核普通分配，可能睡眠 */
#define GFP_ATOMIC  0x0002  /* 原子上下文，不可睡眠 */

/* ======================== 公共 API ======================== */

/* 初始化 slab 子系统（须在 mm_init 之后调用） */
__init void kmem_cache_init(void);

/*
 * 创建一个新的对象缓存
 *   name : 缓存名称（调试用，最长 KMEM_CACHE_NAMELEN-1）
 *   size : 每个对象的字节数
 *
 * 返回: 缓存描述符指针，失败返回 NULL
 */
kmem_cache_t *kmem_cache_create(const char *name, unsigned int size);

/*
 * 从缓存中分配一个对象
 * 若无可用 slab，自动向伙伴系统申请新页（kmem_cache_grow）
 */
void *kmem_cache_alloc(kmem_cache_t *cache);

/*
 * 释放对象回所属缓存
 * 自动通过 virt_to_page + page->virtual 反查 slab 描述符
 */
void kmem_cache_free(kmem_cache_t *cache, void *obj);

/* ======================== 通用分配器（kmalloc / kfree） ========================
 *
 * 参考 Linux 2.6.20 slab_def.h 的 malloc_sizes 设计：
 *   维护 9 个通用大小缓存：32/64/128/256/512/1024/2048/4096/8192 字节
 *   kmalloc 从中选取最小满足 size 的缓存分配
 *   大小 == 8192 时使用 2 阶页面分配（8KB 对齐，满足 THREAD_SIZE 对齐需求）
 *
 * malloc_caches[9] 由 kmem_cache_init() 末尾创建
 */
extern kmem_cache_t *malloc_caches[9];

/*
 * kmalloc - 通用内存分配
 *   size  : 需要分配的字节数
 *   flags : GFP_KERNEL / GFP_ATOMIC
 * 返回: 对齐的内存指针，失败返回 NULL
 */
void *kmalloc(unsigned int size, unsigned int flags);

/*
 * kfree - 释放 kmalloc 分配的内存
 *   obj : kmalloc 返回的指针，NULL 安全
 */
void kfree(const void *obj);

#endif /* __SLAB_H__ */
