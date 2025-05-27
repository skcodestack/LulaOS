#define MB_MAGIC 0x1BADB002
#define MB_FLAGS 0x00000003 // 4K aligned, memory map
#define MB_CHKSUM -(MB_MAGIC + MB_FLAGS)
