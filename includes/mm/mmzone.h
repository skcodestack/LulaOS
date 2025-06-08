#ifndef __MMZONE_H__
#define __MMZONE_H__
#include <mm/bootmem.h>
#include <mm/mm.h>

#define LONG_ALIGN(x) (((x)+(sizeof(long))-1)&~((sizeof(long))-1))

#define MAX_DMA_ADDRESS   (PAGE_OFFSET+0x1000000) ///DMA zone size: 16M

#define ZONE_DMA		0
#define ZONE_NORMAL		1
#define ZONE_HIGHMEM		2
#define MAX_NR_ZONES		3

#define __GFP_DMA	0x01
#define __GFP_HIGHMEM	0x02
#define GFP_ZONEMASK	0x0f

#define MAX_ORDER 10

typedef struct free_area_struct {
	struct list_head	free_list;//page list
	unsigned long		*map;//位图
} free_area_t;


/**
 * ZONE_DMA  < 16M
 * ZONE_NORMAL  16M - 896M
 * ZONE_HIGHMEM  > 896M
 */ 
typedef struct zone_struct {
    
	unsigned long	free_pages;//区中现有空闲的个数  
	free_area_t		free_area[MAX_ORDER];//伙伴分配系统中位图数组和页面链表,1，2，3，4.... 2的MAX_ORDER次方 
	struct pglist_data	*zone_pgdat; //本管理区所在的存储节点 
	struct page		*zone_mem_map; //本管理区的内存映射表 管理区在全局mem_map中第一页
	unsigned long	zone_start_paddr;//管理区的起始物理地址 页面号
	unsigned long	zone_start_mapnr;//mme_map中的下标 mem_map中的页面偏移

	char			*name;
	unsigned long	size;//物理内存总页面数
} zone_t;

typedef struct zonelist_struct {
	zone_t * zones [MAX_NR_ZONES+1]; // NULL delimited
} zonelist_t;

typedef struct pglist_data {
    zone_t node_zones[MAX_NR_ZONES];
    zonelist_t node_zonelists[GFP_ZONEMASK+1]; // NULL delimited
    unsigned  nr_zones;
    struct page * node_mem_map;//所有内存的page首地址,节点page数组，被放置在全局MEM_MAP数组
    bootmem_data_t * bdata;

    unsigned long node_start_paddr;//节点起始物理地址 页面号
	unsigned long node_start_mapnr;//该节点在mem_map/node_mem_map中的页面偏移
	unsigned long node_size;//管理区页面总和
    
} pg_data_t;


extern pg_data_t contig_page_data;  

#define NODE_DATA		(&contig_page_data)
#define NODE_MEM_MAP	mem_map


#define MAP_ALIGN(x)	((((x) % sizeof(mem_map_t)) == 0) ? (x) : ((x) + \
		sizeof(mem_map_t) - ((x) % sizeof(mem_map_t))))

void __init zone_init();
#endif