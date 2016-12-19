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
static int kcp_flushall();
static int kcp_flushsndbuf(const void *data, size_t size);

ssize_t
kcp_send(kcp_tunnel_t *tunnel, const void *data, size_t size)
{
    
}


/* function for kcp tunnel group */

int
kcp_group_init(kcp_tunnel_group_t *group)
{
    ngx_connection_t    *c;
    ngx_event_t         *rev, *wev;
    ngx_pmap_conf_t     *pcf;
    ngx_socket_t         s;
    ngx_int_t            event;

    s = ngx_socket(AF_INET, SOCK_DGRAM, 0);

    ngx_log_error(NGX_LOG_INFO, group->log, 0,
                  "create kcp tunnel group! UDP socket %d", s);

    if ((ngx_socket_t)-1 == s) {
        ngx_log_error(NGX_LOG_ERR, group->log, ngx_socket_errno,
                      ngx_socket_n " failed");
        return NGX_ERROR;
    }

    /* get connection */

    c = ngx_get_connection(s, group->log);

    if (NULL == c) {
        if (ngx_close_socket(s) == -1) {
            ngx_log_error(NGX_LOG_ERR, group->log, ngx_socket_errno,
                          ngx_close_socket_n "failed");
        }

        return NGX_ERROR;
    }

    if (ngx_nonblocking(s) == -1) {
        ngx_log_error(NGX_LOG_ERR, group->log, ngx_socket_errno,
                      ngx_nonblocking_n " failed");

        goto failed;
    }

    group->udp_conn = c;

    /* bind address for server */
    
    pcf = ngx_pmap_get_conf(group->cycle->conf_ctx, ngx_pmap_core_module);
    group->ep = pcf->endpoint;
    if (NGX_PMAP_ENDPOINT_SERVER == group->ep) {
        
        if (bind(s, &group->addr.u.sockaddr, group->addr.socklen) < 0) {
            
            ngx_log_error(NGX_LOG_ERR, group->log, ngx_socket_errno,
                          ngx_close_socket_n "failed");
            
            goto failed;
        }
    }

    /* register read event */

    rev = c->read;    
    wev = c->write;
    
    rev->log = group->log;
    rev->handler = kcp_tunnel_group_on_recv;
            
    wev->ready = 1; /* UDP sockets are always ready to write */
    wev->log = group->log;

    c->number = ngx_atomic_fetch_add(ngx_connection_counter, 1);
    c->data = group;

    event = (ngx_event_flags & NGX_USE_CLEAR_EVENT) ?
            NGX_CLEAR_EVENT: /* kqueue, epoll */
            NGX_LEVEL_EVENT; /* select, poll, /dev/poll */

    if (ngx_add_event(rev, NGX_READ_EVENT, event) != NGX_OK) {
        goto failed;
    }

    /* init rbtree(it's used to store kcp tunnel) */
    
    ngx_rbtree_init(&group->rbtree, &group->sentinel, ngx_rbtree_insert_value);

    return NGX_OK;

failed:
    ngx_close_connection(c);
    group->udp_conn = NULL;

    return NGX_ERROR;
}

kcp_tunnel_t *
kcp_create_tunnel(kcp_tunnel_group_t *group, IUINT32 conv)
{    
    kcp_tunnel_t    *tun;
    kcp_arg_t       *arg;

    if (kcp_find_tunnel(group, conv)) {
        ngx_log_error(NGX_LOG_ERR, group->log, 0,
                      "kcp tunnel already exist! conv=%u", conv);
        return NULL;
    }

    tun = ngx_palloc(group->pool, sizeof(kcp_tunnel_t));
    if (NULL == tun) {
        return NULL;
    }

    tun->conv         = conv;    
    tun->group        = group;
    tun->cache        = NULL;
    tun->addr_settled = 0;

    /* create kcp */
    
    tun->kcpcb = ikcp_create(conv, tun);
    if (NULL == tun->kcpcb) {
        ngx_log_error(NGX_LOG_ERR, group->log, 0,
                      "ikcp_create()  failed! conv=%u", conv);
        return NULL;
    }

    tun->kcpcb->output = kcp_output_handler;

    arg = &kcp_prefab_args[2];
    ikcp_nodelay(tun->kcpcb, arg->nodelay, arg->interval, arg->resend, arg->nc);
    ikcp_setmtu(tun->kcpcb, arg->mtu);    

    /* insert kcp to the rbtree */
    
    ngx_log_error(NGX_LOG_INFO, group->log, 0,
                  "create kcp tunnel! conv=%u", conv);
    ngx_rbtree_insert(&group->rbtree, &tun->node);

    return tun;
}

void
kcp_destroy_tunnel(kcp_tunnel_group_t *group, kcp_tunnel_t *tunnel)
{
    ngx_rbtree_delete(&group->rbtree, &tunnel->node);

    ikcp_release(tunnel->kcpcb);
    ngx_pfree(group->pool, tunnel);
}

kcp_tunnel_t *
kcp_find_tunnel(kcp_tunnel_group_t *group, IUINT32 conv)
{
    ngx_rbtree_node_t  *node, *sentinel;

    node = &group->rbtree.root;
    sentinel = &group->sentinel;

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
