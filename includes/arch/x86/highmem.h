#ifndef __HIGHTMEM_H__
#define __HIGHTMEM_H__

#include <thread.h>
#include <arch/x86/page.h>
#include <arch/linkage.h>

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
    __end_of_fixed_addresses
};

// index to virtual address
#define fix_to_virt(x) (FIXADDR_TOP - ((x) << PAGE_SHIFT))

void __set_fixmap (enum fixed_addresses idx,
					unsigned long phys, pgprot_t flags);

#define set_fixmap(idx, phys) \
		__set_fixmap(idx, phys, PAGE_KERNEL)

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

#endif