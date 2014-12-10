
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#ifndef _NGX_RADIX_TREE_H_INCLUDED_
#define _NGX_RADIX_TREE_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>


#define NGX_RADIX_NO_VALUE   (uintptr_t) -1

typedef struct ngx_radix_node_s  ngx_radix_node_t;

struct ngx_radix_node_s {
    ngx_radix_node_t  *right;
    ngx_radix_node_t  *left;
    ngx_radix_node_t  *parent;
    uintptr_t          value;
};

/*
		在ngx_radix_tree结构体中，除root以外的几个字段都是为了对该基树所使用的内存进行管理所做的设计，free
	字段下挂载的是当前空闲的树节点(即从树里删除出来而没被使用的废弃节点，这个节点所占内存空间既没有返还给内存池，
	也没有返还给系统)。这些节点以单链表的形式组织起来(把节点描述结构ngx_radix_node_t的right字段当链表的next
	字段使用)，所以在节点申请函数ngx_radix_alloc()里，会先去这个空闲链表查找是否有废弃节点可用。如果有，就
	直接取链头节点返回;否则就要申请(如果之前没申请过内存页或者上次剩余内存不足一个基树节点)一页内存(ngx_pagesize
	大小，有对齐处理)，然后从中分出一个待分配的基树节点，剩下内存的起始地址和大小分别在tree->start和tree->size
	字段里，以便下次分配基树节点是，可以从剩余内存里直接获取。注：基树相关代码中没有释放页内存并不会导致内存泄露，
	因为这些基树内存的最终回收会在Nginx内存池里处理。
	*/
typedef struct {
    ngx_radix_node_t  *root;
    ngx_pool_t        *pool;
    ngx_radix_node_t  *free;
    char              *start;
    size_t             size;
} ngx_radix_tree_t;


ngx_radix_tree_t *ngx_radix_tree_create(ngx_pool_t *pool,
    ngx_int_t preallocate);

ngx_int_t ngx_radix32tree_insert(ngx_radix_tree_t *tree,
    uint32_t key, uint32_t mask, uintptr_t value);
ngx_int_t ngx_radix32tree_delete(ngx_radix_tree_t *tree,
    uint32_t key, uint32_t mask);
uintptr_t ngx_radix32tree_find(ngx_radix_tree_t *tree, uint32_t key);

#if (NGX_HAVE_INET6)
ngx_int_t ngx_radix128tree_insert(ngx_radix_tree_t *tree,
    u_char *key, u_char *mask, uintptr_t value);
ngx_int_t ngx_radix128tree_delete(ngx_radix_tree_t *tree,
    u_char *key, u_char *mask);
uintptr_t ngx_radix128tree_find(ngx_radix_tree_t *tree, u_char *key);
#endif


#endif /* _NGX_RADIX_TREE_H_INCLUDED_ */
