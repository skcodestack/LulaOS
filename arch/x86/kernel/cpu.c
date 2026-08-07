#include <arch/x86/cpu.h>
#include <printk.h>
void init_cpu(){

    unsigned int eax, ebx, ecx, edx;
    cpuid(0,&eax, &ebx, &ecx, &edx);
    char FactoryName[17] = {0};
    /* CPUID leaf 0: vendor string order = ebx | edx | ecx
     * e.g. "GenuineIntel" = 'Genu' | 'ineI' | 'ntel' */
    *(unsigned int*)&FactoryName[0] = ebx;
    *(unsigned int*)&FactoryName[4] = edx;
    *(unsigned int*)&FactoryName[8] = ecx;
    FactoryName[12] = '\0';
    printk("CPU Vendor: %s\n", FactoryName);

    /* CPUID leaves 0x80000002-0x80000004: 48-byte brand string */
    char BrandName[49] = {0};
    for(unsigned long i = 0x80000002; i < 0x80000005; i++){
        cpuid(i, &eax, &ebx, &ecx, &edx);
        unsigned int off = (i - 0x80000002) * 16;
        *(unsigned int*)&BrandName[off + 0]  = eax;
        *(unsigned int*)&BrandName[off + 4]  = ebx;
        *(unsigned int*)&BrandName[off + 8]  = ecx;
        *(unsigned int*)&BrandName[off + 12] = edx;
    }
    BrandName[48] = '\0';
    printk("CPU Brand: %s\n", BrandName);

    /* CPUID leaf 1: Family / Model / Stepping */
    cpuid(1, &eax, &ebx, &ecx, &edx);
    printk("Family Code:%#x, Extended Family:%#x, "
           "Model Number:%#x, Extended Model:%#x, "
           "Processor Type:%#x, Stepping ID:%#x\n",
           (eax >> 8)  & 0xf,    /* Family Code      bits 11-8  */
           (eax >> 20) & 0xff,   /* Extended Family  bits 27-20 */
           (eax >> 4)  & 0xf,    /* Model Number     bits  7-4  */
           (eax >> 16) & 0xf,    /* Extended Model   bits 19-16 */
           (eax >> 12) & 0x3,    /* Processor Type   bits 13-12 */
           (eax)       & 0xf);   /* Stepping ID      bits  3-0  */

    /* CPUID leaf 0x80000008: address size */
    cpuid(0x80000008, &eax, &ebx, &ecx, &edx);
    printk("Physical Address size:%d bits, Linear Address size:%d bits\n",
           (eax & 0xff), (eax >> 8) & 0xff);

    /* max cpuid operation code */
    cpuid(0, &eax, &ebx, &ecx, &edx);
    printk("MAX Basic Operation Code:%#x\t", eax);

    cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
    printk("MAX Extended Operation Code:%#x\n", eax);
}