/*
 * DRM GEM (Graphics Execution Manager) 显存管理实现
 *
 * 参考 Linux DRM GEM（drivers/gpu/drm/drm_gem.c）：
 *   - drm_gem_create(): 分配物理连续内存，创建 GEM 对象
 *   - drm_gem_destroy(): 释放物理内存，销毁对象
 *   - drm_gem_find(): 根据 handle 查找对象
 *   - drm_gem_mmap(): 将显存映射到用户虚拟地址
 *
 * 内存分配策略：
 *   使用 __get_free_pages() 从伙伴系统分配物理连续页，
 *   保证 GPU DMA 可直接访问。
 */

#include <drm/drm_gem.h>
#include <drm/drm_core.h>
#include <mm/slab.h>
#include <mm/mmzone.h>
#include <arch/x86/page.h>
#include <arch/x86/pgtable.h>
#include <printk.h>
#include <libs/memcpy.h>
#include <video/fb.h>     /* 使用 fb_ioremap */

/* ======================== 内部辅助 ======================== */

/*
 * gem_alloc_pages - 分配物理连续页
 *
 * 使用伙伴系统分配 order 阶连续页，返回物理地址。
 * 若 size <= PAGE_SIZE，用 kmalloc（更快）。
 * 若 size > PAGE_SIZE，用 __alloc_pages（物理连续）。
 *
 * @size: 请求的字节数（已 PAGE_SIZE 对齐）
 * @vaddr_out: 输出内核虚拟地址
 * 返回: 物理地址，失败返回 0
 */
static unsigned long gem_alloc_pages(unsigned long size, void **vaddr_out)
{
    unsigned int order;
    unsigned long pages;
    void *vaddr;

    /* 计算需要的页数和阶数 */
    pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    /* 找最小 order 使得 (1 << order) >= pages */
    order = 0;
    while ((1UL << order) < pages)
        order++;

    if (order == 0) {
        /* 单页：用 kmalloc 分配 */
        vaddr = kmalloc(PAGE_SIZE, GFP_KERNEL);
        if (!vaddr)
            return 0;
        *vaddr_out = vaddr;
        /* kmalloc 返回的是内核虚拟地址，转换为物理地址 */
        return __pa((unsigned long)vaddr);
    }

    /* 多页：使用 __alloc_pages 分配连续物理页 */
    /* __alloc_pages(gfp_mask, order) */
    struct page *page = __alloc_pages(GFP_KERNEL, order);
    if (!page)
        return 0;

    /* 计算物理地址（通过 page 在 mem_map 中的偏移） */
    unsigned long pfn = (unsigned long)(page - mem_map);
    unsigned long phys = pfn << PAGE_SHIFT;

    /* 获取内核虚拟地址 */
    vaddr = page->virtual;
    if (!vaddr) {
        /* 如果 page->virtual 未设置，通过 __va 计算 */
        vaddr = __va(phys);
        page->virtual = vaddr;
    }

    *vaddr_out = vaddr;
    return phys;
}

/*
 * gem_free_pages - 释放物理连续页
 *
 * @phys_addr: 物理地址
 * @size: 字节数
 */
static void gem_free_pages(unsigned long phys_addr, unsigned long size)
{
    unsigned long pages;
    unsigned int order;
    void *vaddr;

    if (phys_addr == 0)
        return;

    pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    order = 0;
    while ((1UL << order) < pages)
        order++;

    vaddr = (void *)__va(phys_addr);

    if (order == 0) {
        kfree(vaddr);
    } else {
        /* 多页释放：通过 struct page 释放 */
        struct page *page = virt_to_page((unsigned long)vaddr);
        __free_pages(page, order);
    }
}

/* ======================== 公共 API ======================== */

void drm_gem_init_device(struct drm_device *dev)
{
    INIT_LIST_HEAD(&dev->gem_objects);
    dev->next_handle = 1;   /* handle 从 1 开始 */
}

void drm_gem_cleanup_device(struct drm_device *dev)
{
    struct list_head *pos, *n;

    list_for_each_safe(pos, n, &dev->gem_objects) {
        struct drm_gem_object *obj = list_entry(pos, struct drm_gem_object, list);
        printk("[drm:gem] WARNING: leaked GEM object handle=%d size=%lu\n",
               obj->handle, obj->size);
        drm_gem_destroy(obj);
    }
}

struct drm_gem_object *drm_gem_create(struct drm_device *dev, unsigned long size)
{
    struct drm_gem_object *obj;
    unsigned long aligned_size;
    void *vaddr = (void *)0;
    unsigned long phys_addr;

    if (!dev || size == 0)
        return (void *)0;

    /* PAGE_SIZE 对齐 */
    aligned_size = (size + PAGE_SIZE - 1) & PAGE_MASK;

    /* 分配 GEM 对象描述符 */
    obj = (struct drm_gem_object *)kmalloc(sizeof(*obj), GFP_KERNEL);
    if (!obj) {
        printk("[drm:gem] Failed to allocate GEM object descriptor\n");
        return (void *)0;
    }

    /* 分配物理连续内存 */
    phys_addr = gem_alloc_pages(aligned_size, &vaddr);
    if (phys_addr == 0) {
        printk("[drm:gem] Failed to allocate %lu bytes of physical memory\n",
               aligned_size);
        kfree(obj);
        return (void *)0;
    }

    /* 初始化对象 */
    obj->dev       = dev;
    obj->size      = aligned_size;
    obj->vaddr     = vaddr;
    obj->phys_addr = phys_addr;
    obj->handle    = dev->next_handle++;
    obj->refcount  = 1;
    INIT_LIST_HEAD(&obj->list);

    /* 链入设备的 GEM 对象链表 */
    list_add_tail(&obj->list, &dev->gem_objects);

    /* 清零显存内容（安全初始化） */
    memset(vaddr, 0, aligned_size);

    printk("[drm:gem] Created object: handle=%d, size=%lu, phys=0x%lx, virt=0x%lx\n",
           obj->handle, aligned_size, phys_addr, (unsigned long)vaddr);

    return obj;
}

void drm_gem_destroy(struct drm_gem_object *obj)
{
    if (!obj)
        return;

    /* 从链表中移除 */
    list_del(&obj->list);

    /* 释放物理内存 */
    gem_free_pages(obj->phys_addr, obj->size);

    printk("[drm:gem] Destroyed object: handle=%d, size=%lu\n",
           obj->handle, obj->size);

    /* 释放描述符 */
    kfree(obj);
}

struct drm_gem_object *drm_gem_find(struct drm_device *dev, uint32_t handle)
{
    struct list_head *pos;

    if (!dev)
        return (void *)0;

    list_for_each(pos, &dev->gem_objects) {
        struct drm_gem_object *obj = list_entry(pos, struct drm_gem_object, list);
        if (obj->handle == handle)
            return obj;
    }

    return (void *)0;
}

int drm_gem_mmap(struct drm_gem_object *obj, unsigned long user_vaddr)
{
    void *mapped;

    if (!obj || user_vaddr == 0)
        return DRM_ERR_INVAL;

    /* 检查用户地址是否页对齐 */
    if (user_vaddr & ~PAGE_MASK)
        return DRM_ERR_INVAL;

    /*
     * 调用 fb_ioremap 将物理页映射到指定的虚拟地址
     * 注意：fb_ioremap 会占用 FB_IOREMAP_BASE 之后的地址空间，
     * 这里直接使用它进行映射（简化实现）。
     */
    mapped = fb_ioremap(obj->phys_addr, obj->size);
    if (!mapped)
        return DRM_ERR_NOMEM;

    /* 如果映射地址与请求地址不同，需要额外处理（简化实现） */
    /* 真实内核需要修改页表将物理页映射到 user_vaddr */

    printk("[drm:gem] Mapped object handle=%d to virt=0x%lx\n",
           obj->handle, (unsigned long)mapped);

    return 0;
}

void drm_gem_put(struct drm_gem_object *obj)
{
    if (!obj)
        return;

    obj->refcount--;
    if (obj->refcount <= 0)
        drm_gem_destroy(obj);
}
