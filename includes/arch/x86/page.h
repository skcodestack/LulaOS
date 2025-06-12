#ifndef __PAGE_H__
#define __PAGE_H__

/** page base info */
#define __PAGE_OFFSET 0xC0000000
#define PAGE_OFFSET __PAGE_OFFSET

#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define PAGE_MASK (~(PAGE_SIZE - 1))
#define PAGE_ALIGN(addr) (((addr) + PAGE_SIZE - 1) & PAGE_MASK)

#ifndef __ASM__  
#include <mm/mm.h>
/** page frame number */
#define PFN_ALIGN(x)	(((unsigned long)(x) + (PAGE_SIZE - 1)) & PAGE_MASK)
#define PFN_UP(x)	(((x) + PAGE_SIZE-1) >> PAGE_SHIFT)
#define PFN_DOWN(x)	((x) >> PAGE_SHIFT)
#define PFN_PHYS(x)	((x) << PAGE_SHIFT)


/** page table op */
typedef struct { unsigned long pgd; } pgd_t;
typedef struct { unsigned long pte_low; } pte_t; 
typedef struct { unsigned long pgprot; } pgprot_t;

#define pgd_val(x)	((x).pgd)
#define pte_val(x)	((x).pte_low)
#define pgprot_val(x)	((x).pgprot)

#define __pgd(x) ((pgd_t){(x)})
#define __pte(x) ((pte_t){(x)})
#define __pgprot(x)	((pgprot_t) { (x) } )

#define set_pgd(pgdptr,pgdval) (*(pgdptr) = pgdval)
#define set_pte(pteptr, pteval) (*(pteptr) = pteval)
#define __mk_pte(page_nr,pgprot) __pte(((page_nr) << PAGE_SHIFT) | pgprot_val(pgprot))

/** address op */
#define __pa(vaddr) ((unsigned long)(vaddr) - PAGE_OFFSET)
#define __va(paddr) ((void *)((unsigned long)(paddr) + PAGE_OFFSET))
#define virt_to_page(kaddr)	(mem_map + (__pa(kaddr) >> PAGE_SHIFT))

#endif


#endif