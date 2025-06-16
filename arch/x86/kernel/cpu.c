#include <arch/x86/cpu.h>
#include <printk.h>
void init_cpu(){

    unsigned int eax, ebx, ecx, edx;
    cpuid(0,&eax, &ebx, &ecx, &edx);
    char FactoryName[17] = {0};
    *(unsigned int*)&FactoryName[0] = ebx;
    *(unsigned int*)&FactoryName[0] = edx;
    *(unsigned int*)&FactoryName[0] = ecx;
    FactoryName[12] = "\0";
    printk("CPU Factory Name: %s\n", FactoryName);

    for(unsigned long i = 0x80000002;i < 0x80000005;i++){
        cpuid(i, &eax, &ebx, &ecx, &edx);

        *(unsigned int*)&FactoryName[0] = eax;

		*(unsigned int*)&FactoryName[4] = ebx;

		*(unsigned int*)&FactoryName[8] = ecx;

		*(unsigned int*)&FactoryName[12] = edx;

		FactoryName[16] = '\0';
        printk("%s\n", FactoryName);
    }

    //Version Informatin Type,Family,Model,and Stepping ID
	cpuid(1, &eax, &ebx, &ecx, &edx);
	printk("Family Code:%#010x,Extended Family:%#010x,Model Number:%#010x,Extended Model:%#010x,Processor Type:%#010x,Stepping ID:%#010x\n",(eax >> 8 & 0xf),(eax >> 20 & 0xff),(eax >> 4 & 0xf),(eax >> 16 & 0xf),(eax & 0xf));

	//get Linear/Physical Address size 
    cpuid(0x80000008, &eax, &ebx, &ecx, &edx);
	printk("Physical Address size:%08d,Linear Address size:%08d\n",(eax & 0xff),(eax >> 8 & 0xff));

	//max cpuid operation code 
    cpuid(0,&eax, &ebx, &ecx, &edx);
	printk("MAX Basic Operation Code :%#010x\t",eax);
    
    cpuid(0x80000000,&eax, &ebx, &ecx, &edx);
	printk("MAX Extended Operation Code :%#010x\n",eax);
}