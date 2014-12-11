
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>


static ngx_radix_node_t *ngx_radix_alloc(ngx_radix_tree_t *tree);

//创建基树的初始函数
ngx_radix_tree_t *
ngx_radix_tree_create(ngx_pool_t *pool, ngx_int_t preallocate)
{
    uint32_t           key, mask, inc;
    ngx_radix_tree_t  *tree;

	//首先分配基树描述结构ngx_radix_tree_t的内存
    tree = ngx_palloc(pool, sizeof(ngx_radix_tree_t));
    if (tree == NULL) {
        return NULL;
    }

    tree->pool = pool;
    tree->free = NULL;
    tree->start = NULL;
    tree->size = 0;

	//然后创建一个只有根节点的基树
    tree->root = ngx_radix_alloc(tree);
    if (tree->root == NULL) {
        return NULL;
    }

    tree->root->right = NULL;
    tree->root->left = NULL;
    tree->root->parent = NULL;
    tree->root->value = NGX_RADIX_NO_VALUE;

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
	
	/*
		创建完只有根节点的基树后，还会根据参数preallocate进行树节点的创建。如果该值指定为0，表示
		不需预创建而直接返回;若preallocate为正数n，则表示要预创建的基树(预创建的是一颗满二叉树)
		深度(假定根节点层次为0，树深度定义为最大的叶节点层次，即若preallocate为2，接下来一共创建
		6个树节点);若preallocate为-1，则表示要选择一个默认深度，根据平台的不同而不同。若为其他
		负数，那就是一个异常输入，需小心（若为-2，几乎创建无数多个树节点而必定导致内存不足而失败）。
	*/
    if (preallocate == 0) {
        return tree;
    }

    /*
     * Preallocation of first nodes : 0, 1, 00, 01, 10, 11, 000, 001, etc.
     * increases TLB hits even if for first lookup iterations.
     * On 32-bit platforms the 7 preallocated bits takes continuous 4K,
     * 8 - 8K, 9 - 16K, etc.  On 64-bit platforms the 6 preallocated bits
     * takes continuous 4K, 7 - 8K, 8 - 16K, etc.  There is no sense to
     * to preallocate more than one page, because further preallocation
     * distributes the only bit per page.  Instead, a random insertion
     * may distribute several bits per page.
     *
     * Thus, by default we preallocate maximum
     *     6 bits on amd64 (64-bit platform and 4K pages)
     *     7 bits on i386 (32-bit platform and 4K pages)
     *     7 bits on sparc64 in 64-bit mode (8K pages)
     *     8 bits on sparc64 in 32-bit mode (8K pages)
     */

    if (preallocate == -1) {
        switch (ngx_pagesize / sizeof(ngx_radix_node_t)) {

        /* amd64 */
        case 128:
            preallocate = 6;
            break;

        /* i386, sparc64 */
        case 256:
            preallocate = 7;
            break;

        /* sparc64 in 32-bit mode */
        default:
            preallocate = 8;
        }
    }

	/*
		背景知识：
		1、Nginx提供的基树仅被geo模块使用，这个模块使用基树来处理IP地址的匹配查找
		2、在nginx-1.2.0版本内，geo模块仅支持IPV4(1.6.2不清楚)，这意味着这颗基树支持的最大
			深度为32就足够了，所以这里的变量key、mask、inc都为uint32_t类型
		3、key与节点的对应是从高位向低位逐步匹配的，因为geo模块里真正使用的IP网络地址，如
			192.168.0.0/16，它们前面bit位才是有效区分位，若从后往前位匹配，会有大量bit 0，导致
			基本任何一个IP网络地址插入到基树都会达到32层。
		4、ngx_radix32tree_insert()和ngx_radix32tree_delete()函数中，有参数key的同时有参数
			mask的原因：与Nginx内基树的应用有关。因为基树只被geo模块使用，而geo模块存储的IP网络
			地址大多只有前面bit位有效，如192.168.0.0/16，只要到16位(即16层)即可，否则的话要到32
			位而白白浪费内存，更糟糕的是无法区分192.168.0.0/16和192.168.0.0/32，而加了参数mask
			就可解决此问题。
	*/
	//树节点的创建
    mask = 0;
    inc = 0x80000000;

	/*
		当第二次进入while(preallocate--){}循环(假设此时为真，即预创建的树深度超过1)后，
	mask值等于0xC0000000，对应2层节点。在内部do(...)while(key)循环内，key值依次是
	0x0、ox40000000、0x80000000、0xC0000000、0x0(因为溢出而得到该值)，前面4次分别
	创建c、d、e、f4个节点，第5次循环退出。
		其他层次节点的创建情况与上类似。
	*/
    while (preallocate--) {

        key = 0;
        mask >>= 1;
		//初始mask值(第一次循环时)，即最高位为1，对应1层节点(根节点对应第0层)
        mask |= 0x80000000;

		/*循环用于创建这一层的所有节点
			如：在第1层时，key首先为0，创建的左节点a，当key+=inc后，即key等于0x80000000，
		此时创建的是右节点b。再执行key+=inc后，由于溢出导致key为0，从而do(...)while(key)
		循环退出。
		*/
        do {
            if (ngx_radix32tree_insert(tree, key, mask, NGX_RADIX_NO_VALUE)
                != NGX_OK)
            {
                return NULL;
            }

            key += inc;

        } while (key);

        inc >>= 1;
    }

    return tree;
}

/*
	注意：基树并不要求是满二叉树，仅仅只是在函数ngx_radix_tree_create()里设定创建
		为满二叉树，但是在其他地方调用函数ngx_radix32tree_insert()进行节点插入时，
		是哪个位置的树节点就从根开始创建到哪个位置，并不会让这颗基树时时刻刻都保持为
		满二叉树。
*/

ngx_int_t
ngx_radix32tree_insert(ngx_radix_tree_t *tree, uint32_t key, uint32_t mask,
    uintptr_t value)
{
    uint32_t           bit;
    ngx_radix_node_t  *node, *next;

    bit = 0x80000000;

    node = tree->root;
    next = tree->root;

    while (bit & mask) {
        if (key & bit) {
            next = node->right;

        } else {
            next = node->left;
        }

        if (next == NULL) {
            break;
        }

        bit >>= 1;
        node = next;
    }

    if (next) {
        if (node->value != NGX_RADIX_NO_VALUE) {
            return NGX_BUSY;
        }

        node->value = value;
        return NGX_OK;
    }

    while (bit & mask) {
        next = ngx_radix_alloc(tree);
        if (next == NULL) {
            return NGX_ERROR;
        }

        next->right = NULL;
        next->left = NULL;
        next->parent = node;
        next->value = NGX_RADIX_NO_VALUE;

        if (key & bit) {
            node->right = next;

        } else {
            node->left = next;
        }

        bit >>= 1;
        node = next;
    }

    node->value = value;

    return NGX_OK;
}


ngx_int_t
ngx_radix32tree_delete(ngx_radix_tree_t *tree, uint32_t key, uint32_t mask)
{
    uint32_t           bit;
    ngx_radix_node_t  *node;

    bit = 0x80000000;
    node = tree->root;

    while (node && (bit & mask)) {
        if (key & bit) {
            node = node->right;

        } else {
            node = node->left;
        }

        bit >>= 1;
    }

    if (node == NULL) {
        return NGX_ERROR;
    }

    if (node->right || node->left) {
        if (node->value != NGX_RADIX_NO_VALUE) {
            node->value = NGX_RADIX_NO_VALUE;
            return NGX_OK;
        }

        return NGX_ERROR;
    }

    for ( ;; ) {
        if (node->parent->right == node) {
            node->parent->right = NULL;

        } else {
            node->parent->left = NULL;
        }

        node->right = tree->free;
        tree->free = node;

        node = node->parent;

        if (node->right || node->left) {
            break;
        }

        if (node->value != NGX_RADIX_NO_VALUE) {
            break;
        }

        if (node->parent == NULL) {
            break;
        }
    }

    return NGX_OK;
}


//不需要mask参数的原因：它是最长匹配，而且利用key值一直往下匹配时，遇到空节点会自然停止。
uintptr_t
ngx_radix32tree_find(ngx_radix_tree_t *tree, uint32_t key)
{
    uint32_t           bit;
    uintptr_t          value;
    ngx_radix_node_t  *node;

    bit = 0x80000000;
    value = NGX_RADIX_NO_VALUE;
    node = tree->root;

    while (node) {
        if (node->value != NGX_RADIX_NO_VALUE) {
            value = node->value;
        }

        if (key & bit) {
            node = node->right;

        } else {
            node = node->left;
        }

        bit >>= 1;
    }

    return value;
}


#if (NGX_HAVE_INET6)

ngx_int_t
ngx_radix128tree_insert(ngx_radix_tree_t *tree, u_char *key, u_char *mask,
    uintptr_t value)
{
    u_char             bit;
    ngx_uint_t         i;
    ngx_radix_node_t  *node, *next;

    i = 0;
    bit = 0x80;

    node = tree->root;
    next = tree->root;

    while (bit & mask[i]) {
        if (key[i] & bit) {
            next = node->right;

        } else {
            next = node->left;
        }

        if (next == NULL) {
            break;
        }

        bit >>= 1;
        node = next;

        if (bit == 0) {
            if (++i == 16) {
                break;
            }

            bit = 0x80;
        }
    }

    if (next) {
        if (node->value != NGX_RADIX_NO_VALUE) {
            return NGX_BUSY;
        }

        node->value = value;
        return NGX_OK;
    }

    while (bit & mask[i]) {
        next = ngx_radix_alloc(tree);
        if (next == NULL) {
            return NGX_ERROR;
        }

        next->right = NULL;
        next->left = NULL;
        next->parent = node;
        next->value = NGX_RADIX_NO_VALUE;

        if (key[i] & bit) {
            node->right = next;

        } else {
            node->left = next;
        }

        bit >>= 1;
        node = next;

        if (bit == 0) {
            if (++i == 16) {
                break;
            }

            bit = 0x80;
        }
    }

    node->value = value;

    return NGX_OK;
}


ngx_int_t
ngx_radix128tree_delete(ngx_radix_tree_t *tree, u_char *key, u_char *mask)
{
    u_char             bit;
    ngx_uint_t         i;
    ngx_radix_node_t  *node;

    i = 0;
    bit = 0x80;
    node = tree->root;

    while (node && (bit & mask[i])) {
        if (key[i] & bit) {
            node = node->right;

        } else {
            node = node->left;
        }

        bit >>= 1;

        if (bit == 0) {
            if (++i == 16) {
                break;
            }

            bit = 0x80;
        }
    }

    if (node == NULL) {
        return NGX_ERROR;
    }

    if (node->right || node->left) {
        if (node->value != NGX_RADIX_NO_VALUE) {
            node->value = NGX_RADIX_NO_VALUE;
            return NGX_OK;
        }

        return NGX_ERROR;
    }

    for ( ;; ) {
        if (node->parent->right == node) {
            node->parent->right = NULL;

        } else {
            node->parent->left = NULL;
        }

        node->right = tree->free;
        tree->free = node;

        node = node->parent;

        if (node->right || node->left) {
            break;
        }

        if (node->value != NGX_RADIX_NO_VALUE) {
            break;
        }

        if (node->parent == NULL) {
            break;
        }
    }

    return NGX_OK;
}


uintptr_t
ngx_radix128tree_find(ngx_radix_tree_t *tree, u_char *key)
{
    u_char             bit;
    uintptr_t          value;
    ngx_uint_t         i;
    ngx_radix_node_t  *node;

    i = 0;
    bit = 0x80;
    value = NGX_RADIX_NO_VALUE;
    node = tree->root;

    while (node) {
        if (node->value != NGX_RADIX_NO_VALUE) {
            value = node->value;
        }

        if (key[i] & bit) {
            node = node->right;

        } else {
            node = node->left;
        }

        bit >>= 1;

        if (bit == 0) {
            i++;
            bit = 0x80;
        }
    }

    return value;
}

#endif


//基树节点申请函数
static ngx_radix_node_t *
ngx_radix_alloc(ngx_radix_tree_t *tree)
{
    ngx_radix_node_t  *p;

    if (tree->free) {
        p = tree->free;
        tree->free = tree->free->right;
        return p;
    }

    if (tree->size < sizeof(ngx_radix_node_t)) {
        tree->start = ngx_pmemalign(tree->pool, ngx_pagesize, ngx_pagesize);
        if (tree->start == NULL) {
            return NULL;
        }

        tree->size = ngx_pagesize;
    }

    p = (ngx_radix_node_t *) tree->start;
    tree->start += sizeof(ngx_radix_node_t);
    tree->size -= sizeof(ngx_radix_node_t);

    return p;
}
