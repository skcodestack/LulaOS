#ifndef __PGTABLE_H__
#define __PGTABLE_H__

#include <arch/x86/page.h>
 
/* zero page  is shared area that is alwauys zero ,for zero mmap page area */
#define ZERO_PAGE(vaddr) (virt_to_page(empty_zero_page))
extern unsigned long empty_zero_page[1024];

/*  swapper_pg_dir is the kernel page table directory */
extern pgd_t swapper_pg_dir[1024];
extern unsigned long pg0[];

#define PGDIR_SHIFT 22
#define PGDIR_SIZE (1UL << PGDIR_SHIFT)
#define PGDIR_MASK (~(PGDIR_SIZE - 1))

#define PTRS_PER_PGD	1024
#define PTRS_PER_PTE	1024



/* pgd  pte entry attribute */
#define _PAGE_BIT_PRESENT 0
#define _PAGE_BIT_RW 1
#define _PAGE_BIT_USER 2
#define _PAGE_BIT_PWT 3
#define _PAGE_BIT_PCD 4
#define _PAGE_BIT_ACCESSED 5
#define _PAGE_BIT_DIRTY 6
#define _PAGE_BIT_PAT 7
#define _PAGE_BIT_GLOBAL 8
#define _PAGE_BIT_UNUSED1 9
#define _PAGE_BIT_UNUSED2 10
#define _PAGE_BIT_UNUSED3 11

#define _PAGE_PRESENT (1 << _PAGE_BIT_PRESENT)
#define _PAGE_RW (1 << _PAGE_BIT_RW)
#define _PAGE_USER (1 << _PAGE_BIT_USER)
#define _PAGE_PWT (1 << _PAGE_BIT_PWT)
#define _PAGE_PCD (1 << _PAGE_BIT_PCD)
#define _PAGE_ACCESSED (1 << _PAGE_BIT_ACCESSED)
#define _PAGE_DIRTY (1 << _PAGE_BIT_DIRTY)
#define _PAGE_PAT (1 << _PAGE_BIT_PAT)
#define _PAGE_GLOBAL (1 << _PAGE_BIT_GLOBAL)
#define _PAGE_UNUSED1 (1 << _PAGE_BIT_UNUSED1)
#define _PAGE_UNUSED2 (1 << _PAGE_BIT_UNUSED2)
#define _PAGE_UNUSED3 (1 << _PAGE_BIT_UNUSED3)

#define _PAGE_TABLE	(_PAGE_PRESENT | _PAGE_RW | _PAGE_USER | _PAGE_ACCESSED | _PAGE_DIRTY)
#define _KERNPG_TABLE	(_PAGE_PRESENT | _PAGE_RW | _PAGE_ACCESSED | _PAGE_DIRTY)

/* unused */
#define PAGE_NONE __pgprot(0)
#define PAGE_SHARED __pgprot(_PAGE_PRESENT | _PAGE_RW | _PAGE_USER | _PAGE_ACCESSED) 
#define PAGE_COPY __pgprot(_PAGE_PRESENT | _PAGE_USER | _PAGE_ACCESSED)
#define PAGE_READONLY __pgprot(_PAGE_PRESENT | _PAGE_USER | _PAGE_ACCESSED)

#define _PAGE_KERNEL __pgprot(_PAGE_PRESENT | _PAGE_RW | _PAGE_DIRTY | _PAGE_ACCESSED)
#define _PAGE_KERNEL_RO  (_PAGE_KERNEL & ~_PAGE_RW)
#define _PAGE_KERNEL_NOCACHE (_PAGE_KERNEL | _PAGE_PCD)

#define PAGE_KERNEL		__pgprot(_PAGE_KERNEL)
#define PAGE_KERNEL_RO		__pgprot(_PAGE_KERNEL_RO) 
#define PAGE_KERNEL_NOCACHE	__pgprot(_PAGE_KERNEL_NOCACHE) 

#define pte_present(x) ((x).pte_low & _PAGE_PRESENT)

 
static inline int pte_user(pte_t pte)		{ return (pte).pte_low & _PAGE_USER; }
static inline int pte_read(pte_t pte)		{ return (pte).pte_low & _PAGE_USER; }
static inline int pte_dirty(pte_t pte)		{ return (pte).pte_low & _PAGE_DIRTY; }
static inline int pte_young(pte_t pte)		{ return (pte).pte_low & _PAGE_ACCESSED; }
static inline int pte_write(pte_t pte)		{ return (pte).pte_low & _PAGE_RW; } 


static inline pte_t pte_rdprotect(pte_t pte)	{ (pte).pte_low &= ~_PAGE_USER; return pte; }
static inline pte_t pte_exprotect(pte_t pte)	{ (pte).pte_low &= ~_PAGE_USER; return pte; }
static inline pte_t pte_mkclean(pte_t pte)	{ (pte).pte_low &= ~_PAGE_DIRTY; return pte; }
static inline pte_t pte_mkold(pte_t pte)	{ (pte).pte_low &= ~_PAGE_ACCESSED; return pte; }
static inline pte_t pte_wrprotect(pte_t pte)	{ (pte).pte_low &= ~_PAGE_RW; return pte; }
static inline pte_t pte_mkread(pte_t pte)	{ (pte).pte_low |= _PAGE_USER; return pte; }
static inline pte_t pte_mkexec(pte_t pte)	{ (pte).pte_low |= _PAGE_USER; return pte; }
static inline pte_t pte_mkdirty(pte_t pte)	{ (pte).pte_low |= _PAGE_DIRTY; return pte; }
static inline pte_t pte_mkyoung(pte_t pte)	{ (pte).pte_low |= _PAGE_ACCESSED; return pte; }
static inline pte_t pte_mkwrite(pte_t pte)	{ (pte).pte_low |= _PAGE_RW; return pte; }


// #define mk_pte(page, pgprot)	pfn_pte(page_to_pfn(page), (pgprot))

#define pgd_index(address) (((address) >> PGDIR_SHIFT) & (PTRS_PER_PGD-1))
#define pte_index(address) \
		(((address) >> PAGE_SHIFT) & (PTRS_PER_PTE - 1))

#endif
