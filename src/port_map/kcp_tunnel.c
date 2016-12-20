#include "kcp_tunnel.h"

#include <ngx_socket.h>
#include <ngx_event.h>

static kcp_arg_t kcp_prefab_args[] = {
    {0, 30, 2, 0, 1400,},
    {0, 20, 2, 1, 1400,},
    {1, 20, 2, 1, 1400,},
    {1, 10, 2, 1, 1400,},
};

static int kcp_output_handler(const char *buf, int len, ikcpcb *kcp, void *user);
static void kcp_tunnel_group_on_recv(ngx_event_t *rev);


/* function for kcp tunnel */

static int kcp_flushall(kcp_tunnel_t *t);
static int kcp_flushsndbuf(alg_cache_t *c, const void *data, size_t size);
static int kcp_canflush(kcp_tunnel_t *t);

int
kcp_send(kcp_tunnel_t *t, const void *data, size_t size)
{
    if (kcp_canflush(t) &&
        kcp_flushall(t) &&
        kcp_flushsndbuf(t->sndcache, data, size)) {
        
        return True;
    }
    
    return alg_cache_push(t->sndcache, data, size);
}

static int
kcp_output_handler(const char *buf, int len, ikcpcb *kcp, void *user)
{
    return 0;
}

static int
kcp_flushall(kcp_tunnel_t *t)
{   
    if (kcp_canflush(t)) {
        return alg_cache_flushall(t->sndcache);
    }

    return False;
}

static int
kcp_flushsndbuf(alg_cache_t *c, const void *data, size_t size)
{
    kcp_tunnel_t *t;
    ikcpcb       *kcp;
    const char   *ptr;
    size_t        maxlen;

    t = c->data;
    kcp = t->kcp;
    
    if (!kcp_canflush(t)) {
        return False;
    }
    
    ptr = (const char *)data;
    maxlen = kcp->mss<<4;
    for (;;)
    {
        if (size <= maxlen) // in most case
        {
            ikcp_send(kcp, (const char *)ptr, size);
            ++t->sent_count;
            break;
        }
        else
        {
            ikcp_send(kcp, (const char *)ptr, maxlen);
            ++t->sent_count;
            ptr += maxlen;
            size -= maxlen;
        }
    }
    
    return True;
}

static int
kcp_canflush(kcp_tunnel_t *t)        
{
    ikcpcb *kcp = t->kcp;
    
    return ikcp_waitsnd(kcp) < (int)(kcp->snd_wnd<<1);
}

int
kcp_input(kcp_tunnel_t *t, const void *data, size_t size)
{
    int ret = ikcp_input(t->kcp, (const char *)data, size);
    return 0 == ret;
}

ngx_msec_t
kcp_update(kcp_tunnel_t *t, ngx_msec_t curtime)
{
    ikcpcb       *kcp;
    ngx_pool_t   *pool;
    char         *buf;
    int           size;
    ngx_msec_t    nxt_call_time;

    kcp = t->kcp;
    pool  = t->group->pool;
    
    ikcp_update(kcp, curtime);
    
    kcp_flushall(t);
    
    size = ikcp_peeksize(kcp);
    if (size > 0)
    {
        buf = (char *)ngx_palloc(pool, size);
        
        assert(buf != NULL && "ikcp_recv() malloc failed!");
        assert(ikcp_recv(kcp, buf, size) == size);

        ++t->recv_count;
        if (t->recv_handler) {
            t->recv_handler(t, buf, size);
        }

        ngx_pfree(pool, buf);
    }

    nxt_call_time = ikcp_check(kcp, curtime);
    return nxt_call_time > curtime ? nxt_call_time - curtime : 0;
}


/* function for kcp tunnel group */

int
kcp_group_init(kcp_tunnel_group_t *g)
{
    ngx_connection_t    *c;
    ngx_event_t         *rev, *wev;
    ngx_pmap_conf_t     *pcf;
    ngx_socket_t         s;
    ngx_int_t            event;

    s = ngx_socket(AF_INET, SOCK_DGRAM, 0);

    ngx_log_error(NGX_LOG_INFO, g->log, 0,
                  "create kcp tunnel group! UDP socket %d", s);

    if ((ngx_socket_t)-1 == s) {
        ngx_log_error(NGX_LOG_ERR, g->log, ngx_socket_errno,
                      ngx_socket_n " failed");
        return NGX_ERROR;
    }

    /* get connection */

    c = ngx_get_connection(s, g->log);

    if (NULL == c) {
        if (ngx_close_socket(s) == -1) {
            ngx_log_error(NGX_LOG_ERR, g->log, ngx_socket_errno,
                          ngx_close_socket_n "failed");
        }

        return NGX_ERROR;
    }

    if (ngx_nonblocking(s) == -1) {
        ngx_log_error(NGX_LOG_ERR, g->log, ngx_socket_errno,
                      ngx_nonblocking_n " failed");

        goto failed;
    }

    g->udp_conn = c;

    /* bind address for server */
    
    pcf = ngx_pmap_get_conf(g->cycle->conf_ctx, ngx_pmap_core_module);
    g->ep = pcf->endpoint;
    if (NGX_PMAP_ENDPOINT_SERVER == g->ep) {
        
        if (bind(s, &g->addr.u.sockaddr, g->addr.socklen) < 0) {
            
            ngx_log_error(NGX_LOG_ERR, g->log, ngx_socket_errno,
                          ngx_close_socket_n "failed");
            
            goto failed;
        }
    }

    /* register read event */

    rev = c->read;    
    wev = c->write;
    
    rev->log = g->log;
    rev->handler = kcp_tunnel_group_on_recv;
            
    wev->ready = 1; /* UDP sockets are always ready to write */
    wev->log = g->log;

    c->number = ngx_atomic_fetch_add(ngx_connection_counter, 1);
    c->data = g;

    event = (ngx_event_flags & NGX_USE_CLEAR_EVENT) ?
            NGX_CLEAR_EVENT: /* kqueue, epoll */
            NGX_LEVEL_EVENT; /* select, poll, /dev/poll */

    if (ngx_add_event(rev, NGX_READ_EVENT, event) != NGX_OK) {
        goto failed;
    }

    /* init rbtree(it's used to store kcp tunnel) */
    
    ngx_rbtree_init(&g->rbtree, &g->sentinel, ngx_rbtree_insert_value);

    return NGX_OK;

failed:
    ngx_close_connection(c);
    g->udp_conn = NULL;

    return NGX_ERROR;
}

kcp_tunnel_t *
kcp_create_tunnel(kcp_tunnel_group_t *g, IUINT32 conv)
{    
    kcp_tunnel_t    *t;
    kcp_arg_t       *arg;

    if (kcp_find_tunnel(g, conv)) {
        ngx_log_error(NGX_LOG_ERR, g->log, 0,
                      "kcp tunnel already exist! conv=%u", conv);
        return NULL;
    }

    /* create tunnel */

    t = ngx_pcalloc(g->pool, sizeof(kcp_tunnel_t));
    if (NULL == t) {
        return NULL;
    }

    t->conv  = conv;    
    t->group = g;

    t->sndcache = alg_cache_create(kcp_flushsndbuf,
                                ngx_pmap_cache_alloc,
                                ngx_pmap_cache_dealloc,
                                g->pool);

    if (NULL == t->sndcache) {
        ngx_log_error(NGX_LOG_ALERT, g->log, 0,
                      "create cache failed! kcp conv=%u", t->conv);

        ngx_pfree(g->pool, t);

        return NULL;
    }

    t->sndcache->data = t;

    /* create kcp */
    
    t->kcp = ikcp_create(conv, t);
    if (NULL == t->kcp) {
        ngx_log_error(NGX_LOG_ERR, g->log, 0,
                      "ikcp_create()  failed! conv=%u", conv);
        return NULL;
    }

    t->kcp->output = kcp_output_handler;

    arg = &kcp_prefab_args[2];
    ikcp_nodelay(t->kcp, arg->nodelay, arg->interval, arg->resend, arg->nc);
    ikcp_setmtu(t->kcp, arg->mtu);    

    /* insert kcp to the rbtree */
    
    ngx_log_error(NGX_LOG_INFO, g->log, 0,
                  "create kcp tunnel! conv=%u", conv);
    ngx_rbtree_insert(&g->rbtree, &t->node);

    return t;
}

void
kcp_destroy_tunnel(kcp_tunnel_group_t *g, kcp_tunnel_t *t)
{
    ngx_rbtree_delete(&g->rbtree, &t->node);

    ikcp_release(t->kcp);
    ngx_pfree(g->pool, t);
}

kcp_tunnel_t *
kcp_find_tunnel(kcp_tunnel_group_t *g, IUINT32 conv)
{
    ngx_rbtree_node_t  *node, *sentinel;

    node = g->rbtree.root;
    sentinel = &g->sentinel;

    while (node != sentinel && node->key != conv) {
        node = conv < node->key ? node->left : node->right;
    }

    if (node != sentinel)
        return (kcp_tunnel_t *)node;

    return NULL;
}

void
kcp_tunnel_group_on_recv(ngx_event_t *rev)
{}
