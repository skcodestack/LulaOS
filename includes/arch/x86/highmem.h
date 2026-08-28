#ifndef __HIGHTMEM_H__
#define __HIGHTMEM_H__

#include <thread.h>
#include <arch/x86/page.h>
#include <arch/linkage.h>
#include <arch/x86/pgtable.h>

/**
 * 直接映射
 */
extern unsigned long highstart_pfn,highend_pfn;

/**
 * 
 *  固定映射区 
 * 
 */
#define FIXADDR_TOP 0xffffe000 ///4G-8K
 
#define MAX_IO_APICS 8  //io apic size 
enum kmap_types { 
    KM_TYPE_NR
};

enum fixed_addresses {
    /* local apic, vaddr 0xffffe000*/
    FIX_APIC_BASE, 
    /* x IO APICs */
    FIX_IO_APIC_BASE_0,
    FIX_IO_APIC_BASE_END = FIX_IO_APIC_BASE_0 + MAX_IO_APICS-1,
    /** fixed  temp mmap area 固定映射区中的临时映射区*/
    FIX_KMAP_BEGIN,
    FIX_KMAP_END = FIX_KMAP_BEGIN+(KM_TYPE_NR*NR_CPUS)-1,

    FIX_ACPI_BEGIN,
    FIX_ACPI_END = FIX_ACPI_BEGIN + MAX_IO_APICS-1,
    __end_of_fixed_addresses
};

// index to virtual address
#define fix_to_virt(x) (FIXADDR_TOP - ((x) << PAGE_SHIFT))

void __set_fixmap (enum fixed_addresses idx,
					unsigned long phys, pgprot_t flags);

#define set_fixmap(idx, phys) \
		__set_fixmap(idx, phys, PAGE_KERNEL)

#define set_fixmap_nocache(idx, phys) \
		__set_fixmap(idx, phys, PAGE_KERNEL_NOCACHE)

extern pte_t *fixed_kmap_pte;//fix映射区中的临时映射 pte
extern pgprot_t fixed_kmap_prot;//保护权限 PAGE_KERNEL

void fixed_kmap_init(void) __init;
//static inline void *kmap_atomic(struct page *page, enum km_type type)
//static inline void kunmap_atomic(void *kvaddr, enum km_type type)

/**
 * 
 *  永久映射区 
 * 
 */
#define PKMAP_BASE 0xfe000000  /// 4G-32M ---- fix area
#define LAST_PKMAP 1024
extern pte_t *pkmap_page_table;//永久映射区 对应起始pte 4M大小


// static inline void *kmap(struct page *page);
//static inline void kunmap(struct page *page) 

/**
 *
 *  ioremap - IO 内存映射（arch/x86/kernel/mm/ioremap.c）
 *
 *  将任意物理地址范围映射到内核虚拟地址空间（vmalloc 区），
 *  用于访问 MMIO 设备寄存器（如 PCI BAR、MMCFG 配置空间等）。
 *  默认禁用 CPU 缓存（_PAGE_PCD）。
 *
 *  ioremap_nocache: 与 ioremap 等价（LulaOS 默认即 nocache）
 *  iounmap:         释放 ioremap 建立的映射
 */
void *ioremap(unsigned long phys_addr, unsigned long size);
void *ioremap_nocache(unsigned long phys_addr, unsigned long size);
void  iounmap(void *addr);

/**
 *
 *  vmalloc - 分配物理不连续、虚拟连续的内核内存
 *
 *  每页独立从伙伴系统分配，映射到 vmalloc 区连续虚拟地址。
 *  适合需要大块连续虚拟地址但不要求物理连续的场景。
 *  分配出的内存未清零，调用方需自行初始化。
 *
 *  vfree: 释放 vmalloc 分配的内存（读 PTE 获取 PFN 并归还伙伴系统）
 */
void *vmalloc(unsigned long size);
void  vfree(void *addr);

#endif