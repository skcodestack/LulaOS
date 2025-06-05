#include <arch/x86/mb.h>
#include <arch/x86/boot/multiboot.h>
#include <arch/x86/setup.h>
#include <printk.h>

void find_max_pfn(){ 

    unsigned int size = multiboot_params->mmap_length / sizeof(multiboot_memory_map_t);
    multiboot_memory_map_t * mmap =   (multiboot_memory_map_t *)multiboot_params->mmap_addr;
     
    for (unsigned int i = 0; i < size; i++)
    {   
        multiboot_memory_map_t* entry =  (multiboot_memory_map_t *)(mmap+i);
        printk("Start Address: %x, Length: %d M , Size: %x , Type: %d\n",entry->addr_low,entry->len_low >> 10,entry->size,entry->type);


    }
    
}

