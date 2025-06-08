#ifndef __LINKAGE_H__
#define __LINKAGE_H__

#define L1_CACHE_LINE_SIZE 32

#define asmlinkage __attribute__((regparm(0))) //not use register
#define __cache_aligned_  __attribute__((__aligned__(L1_CACHE_LINE_SIZE))) 
#define __inline__ inline

#define __init		__attribute__ ((__section__ (".text.init")))
#define __exit		__attribute__ ((unused, __section__(".text.exit")))
#define __initdata	__attribute__ ((__section__ (".data.init")))
#define __exitdata	__attribute__ ((unused, __section__ (".data.exit")))
#define __initsetup	__attribute__ ((unused,__section__ (".setup.init")))
#define __init_call	__attribute__ ((unused,__section__ (".initcall.init")))
#define __exit_call	__attribute__ ((unused,__section__ (".exitcall.exit")))


#define SYMBOL_NAME(sym) sym
#define SYMBOL_NAME_STR(sym) #sym
#define SYMBOL_NAME_LABEL(sym) sym##:



// .type SYMBOL_NAME(sym),@function;
#define ENTRY(sym)  \
    .globl SYMBOL_NAME(sym); \
    SYMBOL_NAME_LABEL(sym)


#endif