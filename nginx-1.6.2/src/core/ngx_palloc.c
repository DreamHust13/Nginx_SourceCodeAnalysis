
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>


static void *ngx_palloc_block(ngx_pool_t *pool, size_t size);
static void *ngx_palloc_large(ngx_pool_t *pool, size_t size);

//新建内存池
ngx_pool_t *
ngx_create_pool(size_t size, ngx_log_t *log)
{
	//ngx_pool_t类型为Nginx封装的内存池类型
    ngx_pool_t  *p;

	//进行16字节的内存对齐分配(对齐处理一般是为了从性能上做考虑)
    p = ngx_memalign(NGX_POOL_ALIGNMENT, size, log);
    if (p == NULL) {
        return NULL;
    }

	/*
		创建的内存池被结构体ngx_pool_t占去开头一部分(即额外开销)，Nginx实际从该内存池里分配
	空间的起始位置从p->d.last开始，随着内存池空间的对外分配，这个字段的指向会向后移动。
	*/
    p->d.last = (u_char *) p + sizeof(ngx_pool_t);
    p->d.end = (u_char *) p + size;
    p->d.next = NULL;
    p->d.failed = 0;

    size = size - sizeof(ngx_pool_t);
	/*
		细节问题:为什么要将pool->max字段的最大值限制在一页内存?
		
		答:从ngx_palloc()函数中分析可知，这个字段是区分小块内存与大块内存的临界，所以这里的
		   原因也就在于只有当分配的内存空间小于一页时才有缓存的必要(即向Nginx内存池申请)，
		   否则的话，还不如直接利用系统接口malloc()向操作系统申请。
	*/
    p->max = (size < NGX_MAX_ALLOC_FROM_POOL) ? size : NGX_MAX_ALLOC_FROM_POOL;

    p->current = p;
    p->chain = NULL;
    p->large = NULL;
    p->cleanup = NULL;
    p->log = log;

    return p;
}


/*
	内存池的释放问题:
		从代码不难看出,Nginx仅提供对大块内存的释放(通过接口ngx_pfree())，而没有
	提供对小块内存的释放，这意味着从内存池里分配出去的内存不会再回首到内存池里来，而
	只有在销毁整个内存池时，所有这些内存才会回收到系统内存里，这是Nginx内存池一个很重
	要的特点，Nginx很多内存池的设计与处理也都是基于这个特点。
		Nginx内存池这样设计的原因在于Web Server应用的特殊性，即阶段与时效，对于其处理
	的业务逻辑分有明确的阶段，而对每一个阶段又有明确的时效，因此Nginx可针对阶段来分配
	内存池，针对时效来销毁内存池。比如，当一个阶段开始(或其过程中)就创建对应所需的内
	存池，而当这个阶段结束时就销毁其对应的内存池，由于这个阶段有严格的时效性，即在
	一段时间后，其必定会因正常处理、异常错误或超时等而结束，所以不会出现Nginx长时间
	占据大量无用内存池的情况，既然如此，在其阶段过程中回收不用的小块内存自然也就是不
	必要的，等待一段时间一起回收更简单方便。
		内存池的释放，具体实现在ngx_destroy_pool()与ngx_reset_pool()内
*/

void
ngx_destroy_pool(ngx_pool_t *pool)
{
    ngx_pool_t          *p, *n;
    ngx_pool_large_t    *l;
    ngx_pool_cleanup_t  *c;

    for (c = pool->cleanup; c; c = c->next) {
        if (c->handler) {
            ngx_log_debug1(NGX_LOG_DEBUG_ALLOC, pool->log, 0,
                           "run cleanup: %p", c);
            c->handler(c->data);
        }
    }

    for (l = pool->large; l; l = l->next) {

        ngx_log_debug1(NGX_LOG_DEBUG_ALLOC, pool->log, 0, "free: %p", l->alloc);

        if (l->alloc) {
            ngx_free(l->alloc);
        }
    }

#if (NGX_DEBUG)

    /*
     * we could allocate the pool->log from this pool
     * so we cannot use this log while free()ing the pool
     */

    for (p = pool, n = pool->d.next; /* void */; p = n, n = n->d.next) {
        ngx_log_debug2(NGX_LOG_DEBUG_ALLOC, pool->log, 0,
                       "free: %p, unused: %uz", p, p->d.end - p->d.last);

        if (n == NULL) {
            break;
        }
    }

#endif

    for (p = pool, n = pool->d.next; /* void */; p = n, n = n->d.next) {
        ngx_free(p);

        if (n == NULL) {
            break;
        }
    }
}


void
ngx_reset_pool(ngx_pool_t *pool)
{
    ngx_pool_t        *p;
    ngx_pool_large_t  *l;

    for (l = pool->large; l; l = l->next) {
        if (l->alloc) {
            ngx_free(l->alloc);
        }
    }

    for (p = pool; p; p = p->d.next) {
        p->d.last = (u_char *) p + sizeof(ngx_pool_t);
        p->d.failed = 0;
    }

    pool->current = pool;
    pool->chain = NULL;
    pool->large = NULL;
}


//ngx_palloc()尝试从pool内存池里分配size大小的内存空间
void *
ngx_palloc(ngx_pool_t *pool, size_t size)
{
    u_char      *m;
    ngx_pool_t  *p;

	//ngx_create_pool()中，
	//p->max = (size < NGX_MAX_ALLOC_FROM_POOL) ? size : NGX_MAX_ALLOC_FROM_POOL;
	//若size小于等于pool->max(即内存池大小或一页内存(4K-1),小块内存分配),那么就可以从内存
	//池里分配
    if (size <= pool->max) {

        p = pool->current;

        do {
            m = ngx_align_ptr(p->d.last, NGX_ALIGNMENT);

            if ((size_t) (p->d.end - m) >= size) {
                p->d.last = m + size;

                return m;
            }

            p = p->d.next;

        } while (p);

		/*
			从内存池里分配：这个分配不一定来自当前内存池节点，因为有可能当前内存池节点里
		可用的内存大小已经小雨size，如果是这样的话，就需调用函数ngx_palloc_block()申请一个
		新的等同大小的内存池节点，然后从这个新内存池节点里分配出size大小的内存空间。	
		*/
        return ngx_palloc_block(pool, size);
    }

	/*
		若size大于pool->max(即分配大块内存),此时直接"return ngx_palloc_large(pool, size);"
	函数ngx_palloc_large()只能调用系统API接口malloc()向操作系统申请内存，申请的内存块被挂接
	在内存池字段p->large->alloc下(会有对应的管理头结构ngx_pppl_large_t)
	*/
    return ngx_palloc_large(pool, size);
}

//与ngx_palloc()函数实现基本一致，但其取得的内存起始地址没有做对齐处理。
void *
ngx_pnalloc(ngx_pool_t *pool, size_t size)
{
    u_char      *m;
    ngx_pool_t  *p;

    if (size <= pool->max) {

        p = pool->current;

        do {
            m = p->d.last;

            if ((size_t) (p->d.end - m) >= size) {
                p->d.last = m + size;

                return m;
            }

            p = p->d.next;

        } while (p);

        return ngx_palloc_block(pool, size);
    }

    return ngx_palloc_large(pool, size);
}

//ngx_palloc_block()除了申请一个新的等同大小的内存池节点并从这个新内存池节点里分配出
//size大小的内存空间外，还做了两件事。
static void *
ngx_palloc_block(ngx_pool_t *pool, size_t size)
{
    u_char      *m;
    size_t       psize;
    ngx_pool_t  *p, *new, *current;

    psize = (size_t) (pool->d.end - (u_char *) pool);

    m = ngx_memalign(NGX_POOL_ALIGNMENT, psize, pool->log);
    if (m == NULL) {
        return NULL;
    }

    new = (ngx_pool_t *) m;

    new->d.end = m + psize;
    new->d.next = NULL;
    new->d.failed = 0;

    m += sizeof(ngx_pool_data_t);
    m = ngx_align_ptr(m, NGX_ALIGNMENT);
    new->d.last = m + size;

    current = pool->current;

	/*
		首先，把新内存池节点连接到上一个内存池节点的p->d.next字段下形成单链表
		另一件事是根据需要移动内存池描述结构ngx_pool_t的current字段，这个字段记录了后续从该
	内存池分配内存的其实内存池节点，即从这个字段指向的内存池节点开始搜索可分配的内存。
	current字段的变动时根据统计来做的，如果从当前内存池节点分配内存总失败次数(记录在字段
	p->d.failed内)大于等于6次(这是一个经验值，具体判断是"if (p->d.failed++ > 4) {"，由于
	p->d.failed初始值为0，所以当这个判断为真时，至少已经分配失败6次了)，就将current字段移动
	到下一个内存池节点，如果下一个内存池节点的failed统计数也大于等于6次，就再下一个，依次
	如此，如果直到最后仍然是failed统计次数大于等于6次，那么current字段则指向新分配的内存池
	节点。
		(pool->current字段的变动是基于性能的考虑，如果从前面的内存池街店里分配内存总是
	失败，那在下次再进行内存分配时，当然就没有必要再去搜索这些内存池节点，把pool->current
	指向后移，也就是直接跳过它们。)
	*/
    for (p = current; p->d.next; p = p->d.next) {
        if (p->d.failed++ > 4) {
            current = p->d.next;
        }
    }

    p->d.next = new;

    pool->current = current ? current : new;

    return m;
}


static void *
ngx_palloc_large(ngx_pool_t *pool, size_t size)
{
    void              *p;
    ngx_uint_t         n;
    ngx_pool_large_t  *large;

	//函数ngx_palloc_large()只能调用系统API接口malloc()向操作系统申请内存
    p = ngx_alloc(size, pool->log);
    if (p == NULL) {
        return NULL;
    }

    n = 0;

	/*
		申请的内存块被挂接在内存池字段p->large->alloc下(会有对应的管理头结构
	ngx_pool_large_t)如果继续分配大块内存，那么从系统新分配的内存块就以单链表的形式
	继续挂在在内存池字段p->large->alloc下，不过是一种链头插入。
		在内存池的使用过程中，由于大块内存可能会释放(通过函数ngx_pfree())，此时将空出其对应
	的头结构体变量ngx_pool_large_t，所以在进行实际的链头插入操作前，会去搜索当前是否有这种
	情况存在。如果有，则直接把新分配的内存块设置在其alloc指针字段下，综合平衡考虑，这种搜索
	也只是对前面几个链表节点进行。 (if (n++ > 3) {break;})
	*/
    for (large = pool->large; large; large = large->next) {
        if (large->alloc == NULL) {
            large->alloc = p;
            return p;
        }

        if (n++ > 3) {
            break;
        }
    }

    large = ngx_palloc(pool, sizeof(ngx_pool_large_t));
    if (large == NULL) {
        ngx_free(p);
        return NULL;
    }

    large->alloc = p;
    large->next = pool->large;
    pool->large = large;

    return p;
}


//ngx_pmemalign()不管size大小如何，都直接向操作系统申请内存，然后挂接在p->large->alloc字段下。其申请的内存额外有对齐处理。
void *
ngx_pmemalign(ngx_pool_t *pool, size_t size, size_t alignment)
{
    void              *p;
    ngx_pool_large_t  *large;

    p = ngx_memalign(alignment, size, pool->log);
    if (p == NULL) {
        return NULL;
    }

    large = ngx_palloc(pool, sizeof(ngx_pool_large_t));
    if (large == NULL) {
        ngx_free(p);
        return NULL;
    }

    large->alloc = p;
    large->next = pool->large;
    pool->large = large;

    return p;
}


ngx_int_t
ngx_pfree(ngx_pool_t *pool, void *p)
{
    ngx_pool_large_t  *l;

    for (l = pool->large; l; l = l->next) {
        if (p == l->alloc) {
            ngx_log_debug1(NGX_LOG_DEBUG_ALLOC, pool->log, 0,
                           "free: %p", l->alloc);
            ngx_free(l->alloc);
            l->alloc = NULL;

            return NGX_OK;
        }
    }

    return NGX_DECLINED;
}


//ngx_pcalloc()是在ngx_palloc()上的封装，但它在返回分配的内存之前会对这些内存做清零操作。
void *
ngx_pcalloc(ngx_pool_t *pool, size_t size)
{
    void *p;

    p = ngx_palloc(pool, size);
    if (p) {
        ngx_memzero(p, size);
    }

    return p;
}


ngx_pool_cleanup_t *
ngx_pool_cleanup_add(ngx_pool_t *p, size_t size)
{
    ngx_pool_cleanup_t  *c;

    c = ngx_palloc(p, sizeof(ngx_pool_cleanup_t));
    if (c == NULL) {
        return NULL;
    }

    if (size) {
        c->data = ngx_palloc(p, size);
        if (c->data == NULL) {
            return NULL;
        }

    } else {
        c->data = NULL;
    }

    c->handler = NULL;
    c->next = p->cleanup;

    p->cleanup = c;

    ngx_log_debug1(NGX_LOG_DEBUG_ALLOC, p->log, 0, "add cleanup: %p", c);

    return c;
}


void
ngx_pool_run_cleanup_file(ngx_pool_t *p, ngx_fd_t fd)
{
    ngx_pool_cleanup_t       *c;
    ngx_pool_cleanup_file_t  *cf;

    for (c = p->cleanup; c; c = c->next) {
        if (c->handler == ngx_pool_cleanup_file) {

            cf = c->data;

            if (cf->fd == fd) {
                c->handler(cf);
                c->handler = NULL;
                return;
            }
        }
    }
}


void
ngx_pool_cleanup_file(void *data)
{
    ngx_pool_cleanup_file_t  *c = data;

    ngx_log_debug1(NGX_LOG_DEBUG_ALLOC, c->log, 0, "file cleanup: fd:%d",
                   c->fd);

    if (ngx_close_file(c->fd) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_ALERT, c->log, ngx_errno,
                      ngx_close_file_n " \"%s\" failed", c->name);
    }
}


void
ngx_pool_delete_file(void *data)
{
    ngx_pool_cleanup_file_t  *c = data;

    ngx_err_t  err;

    ngx_log_debug2(NGX_LOG_DEBUG_ALLOC, c->log, 0, "file cleanup: fd:%d %s",
                   c->fd, c->name);

    if (ngx_delete_file(c->name) == NGX_FILE_ERROR) {
        err = ngx_errno;

        if (err != NGX_ENOENT) {
            ngx_log_error(NGX_LOG_CRIT, c->log, err,
                          ngx_delete_file_n " \"%s\" failed", c->name);
        }
    }

    if (ngx_close_file(c->fd) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_ALERT, c->log, ngx_errno,
                      ngx_close_file_n " \"%s\" failed", c->name);
    }
}


#if 0

static void *
ngx_get_cached_block(size_t size)
{
    void                     *p;
    ngx_cached_block_slot_t  *slot;

    if (ngx_cycle->cache == NULL) {
        return NULL;
    }

    slot = &ngx_cycle->cache[(size + ngx_pagesize - 1) / ngx_pagesize];

    slot->tries++;

    if (slot->number) {
        p = slot->block;
        slot->block = slot->block->next;
        slot->number--;
        return p;
    }

    return NULL;
}

#endif
