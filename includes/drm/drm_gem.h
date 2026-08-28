/*
 * LulaOS DRM GEM (Graphics Execution Manager) 显存管理
 *
 * 参考 Linux DRM GEM（drivers/gpu/drm/drm_gem.c）实现简化版：
 *   - drm_gem_object: 显存对象，封装一块物理连续内存
 *   - handle 管理: 用简单计数器 + 链表查找实现句柄机制
 *
 * GEM 解决的问题：
 *   1. 多个程序同时使用显存，需要统一管理分配/释放
 *   2. 用户态通过 handle（句柄）访问显存，内核隔离物理地址
 *   3. 支持 CPU 通过 mmap 访问显存内容
 */

#ifndef __DRM_GEM_H__
#define __DRM_GEM_H__

#include <stdint.h>
#include <libs/list.h>

struct drm_device;

/* ======================== GEM 对象 ======================== */

/*
 * drm_gem_object - 显存对象
 *
 * 封装一块物理连续的内存区域，可以是：
 *   - VRAM（显存，GPU 直接访问，速度快）
 *   - 系统内存（通过 GTT/IOMMU 映射给 GPU）
 *
 * 用户态通过 handle（uint32_t）引用此对象，
 * 内核通过 drm_gem_find() 查找具体对象。
 */
struct drm_gem_object {
    struct drm_device *dev;                     /* 所属 DRM 设备 */

    unsigned long size;                         /* 字节数（PAGE_SIZE 对齐） */
    void *vaddr;                                /* 内核虚拟地址 */
    unsigned long phys_addr;                    /* 物理地址 */

    uint32_t handle;                            /* 用户态句柄 */
    int refcount;                               /* 引用计数 */

    struct list_head list;                      /* 链入 drm_device.gem_objects */
};

/* ======================== 公共 API ======================== */

/*
 * drm_gem_create - 分配显存对象
 *
 * 分配 size 字节物理连续内存（PAGE_SIZE 对齐），
 * 返回 GEM 对象指针，并分配 handle。
 *
 * @dev: DRM 设备
 * @size: 请求的字节数
 * 返回: GEM 对象指针，失败返回 NULL
 */
struct drm_gem_object *drm_gem_create(struct drm_device *dev, unsigned long size);

/*
 * drm_gem_destroy - 释放显存对象
 *
 * 释放物理内存并从设备链表中移除
 *
 * @obj: GEM 对象
 */
void drm_gem_destroy(struct drm_gem_object *obj);

/*
 * drm_gem_find - 根据 handle 查找 GEM 对象
 *
 * @dev: DRM 设备
 * @handle: 用户态句柄
 * 返回: GEM 对象指针，未找到返回 NULL
 */
struct drm_gem_object *drm_gem_find(struct drm_device *dev, uint32_t handle);

/*
 * drm_gem_mmap - 将 GEM 对象映射到用户虚拟地址
 *
 * 调用 fb_ioremap() 将物理页映射到指定的用户虚拟地址，
 * 使 CPU 可以直接读写显存内容。
 *
 * @obj: GEM 对象
 * @user_vaddr: 用户指定的虚拟地址（必须页对齐）
 * 返回: 0 表示成功，负数表示错误
 */
int drm_gem_mmap(struct drm_gem_object *obj, unsigned long user_vaddr);

/*
 * drm_gem_get - 增加引用计数
 *
 * @obj: GEM 对象
 */
static inline void drm_gem_get(struct drm_gem_object *obj)
{
    obj->refcount++;
}

/*
 * drm_gem_put - 减少引用计数
 *
 * 引用计数归零时自动释放对象
 *
 * @obj: GEM 对象
 */
void drm_gem_put(struct drm_gem_object *obj);

/* ======================== 内部辅助 ======================== */

/*
 * drm_gem_init_device - 初始化设备的 GEM 子系统
 *
 * 由 drm_core 在创建设备时调用，初始化 gem_objects 链表
 *
 * @dev: DRM 设备
 */
void drm_gem_init_device(struct drm_device *dev);

/*
 * drm_gem_cleanup_device - 清理设备的 GEM 对象
 *
 * 由 drm_core 在销毁设备时调用，释放所有未释放的 GEM 对象
 *
 * @dev: DRM 设备
 */
void drm_gem_cleanup_device(struct drm_device *dev);

#endif /* __DRM_GEM_H__ */
