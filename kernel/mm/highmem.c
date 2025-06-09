#include <arch/x86/highmem.h>
#include <arch/x86/pgtable.h>
#include <arch/x86/page.h>
#include <printk.h>

unsigned long highstart_pfn,highend_pfn;

pte_t *fixed_kmap_pte; 
pgprot_t fixed_kmap_prot;
pte_t *pkmap_page_table;


static inline void set_pte_phys (unsigned long vaddr,
			unsigned long phys, pgprot_t flags)
{  
	pgd_t * pgd = swapper_pg_dir + pgd_index(vaddr);
	if (pgd_none(*pgd)) {
		printk("PAE BUG #00!\n");
		return;
	} 
	pte_t * pte = pte_offset(pgd, vaddr);
	if (pte_val(*pte)){
		printk("PAE BUG #00!\n");
		return;
    }    
	pgprot_t prot = __pgprot(pgprot_val(PAGE_KERNEL) | pgprot_val(flags));
	set_pte(pte, mk_pte_phys(phys, prot)); 
}


void __set_fixmap(enum fixed_addresses idx,
                  unsigned long phys, pgprot_t flags)
{
    unsigned long address = fix_to_virt(idx);

	if (idx >= __end_of_fixed_addresses) {
		printk("Invalid __set_fixmap\n");
		return;
	}
	set_pte_phys(address, phys, flags);
}

__init void fixed_kmap_init(void) {
	unsigned long vaddr =  fix_to_virt(FIX_KMAP_BEGIN);
	pgd_t * pgd = swapper_pg_dir + pgd_index(vaddr);
	fixed_kmap_pte = pte_offset(pgd, vaddr);
	fixed_kmap_prot = PAGE_KERNEL;
}