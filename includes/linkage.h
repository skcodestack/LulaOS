#ifndef __LINKAGE_H__
#define __LINKAGE_H__

#define asmlinkage __attribute__((regparm(0))) //not use register
 
#define SYMBOL_NAME(sym) sym
#define SYMBOL_NAME_STR(sym) #sym
#define SYMBOL_NAME_LABEL(sym) sym##:



#define ENTRY(sym)  \
    .globl SYMBOL_NAME(sym); \
    .type SYMBOL_NAME(sym),@function; \
    SYMBOL_NAME_LABEL(sym):


#endif