#ifndef __PAGE_H__
#define __PAGE_H__

#define __PAGE_OFFSET 0xC0000000
#define PAGE_OFFSET __PAGE_OFFSET

#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define PAGE_MASK (~(PAGE_SIZE - 1))
#define PAGE_ALIGN(addr) (((addr) + PAGE_SIZE - 1) & PAGE_MASK)

 
typedef struct { unsigned long pgd; } pgd_t;
typedef struct { unsigned long pte_low; } pte_t; 
typedef struct { unsigned long pgprot; } pgprot_t;

#define __pgd(x) ((pgd_t){(x)}))
#define __pte(x) ((pte_t){(x)})
#define __pgprot(x)	((pgprot_t) { (x) } )

#define __pa(vaddr) ((unsigned long)(vaddr) - PAGE_OFFSET)
#define __va(paddr) ((void *)((unsigned long)(paddr) + PAGE_OFFSET))




#endif