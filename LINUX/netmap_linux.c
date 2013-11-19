/*
 * Copyright (C) 2013 Universita` di Pisa. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "bsd_glue.h"
#include <linux/file.h>   /* fget(int fd) */

#include <net/netmap.h>
#include <dev/netmap/netmap_kern.h>
#include <dev/netmap/netmap_mem2.h>


/* ========================== LINUX-SPECIFIC ROUTINES ================== */

void netmap_mitigation_init(struct netmap_adapter *na)
{
    hrtimer_init(&na->mit_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    na->mit_timer.function = &generic_timer_handler;
    na->mit_pending = 0;
}

extern unsigned int netmap_generic_mit;

void netmap_mitigation_start(struct netmap_adapter *na)
{
    hrtimer_start(&na->mit_timer, ktime_set(0, netmap_generic_mit), HRTIMER_MODE_REL);
}

void netmap_mitigation_restart(struct netmap_adapter *na)
{
    hrtimer_forward_now(&na->mit_timer, ktime_set(0, netmap_generic_mit));
}

int netmap_mitigation_active(struct netmap_adapter *na)
{
    return hrtimer_active(&na->mit_timer);
}

void netmap_mitigation_cleanup(struct netmap_adapter *na)
{
    hrtimer_cancel(&na->mit_timer);
}

/*
 * This handler is registered within the attached net_device
 * in the Linux RX subsystem, so that every mbuf passed up by
 * the driver can be stolen to the network stack.
 * Stolen packets are put in a queue where the
 * generic_netmap_rxsync() callback can extract them.
 */
rx_handler_result_t linux_generic_rx_handler(struct mbuf **pm)
{
    generic_rx_handler((*pm)->dev, *pm);

    return RX_HANDLER_CONSUMED;
}

/* Ask the Linux RX subsystem to intercept (or stop intercepting)
 * the packets incoming from the interface attached to 'na'.
 */
int
netmap_catch_rx(struct netmap_adapter *na, int intercept)
{
    struct ifnet *ifp = na->ifp;

    if (intercept) {
        return netdev_rx_handler_register(na->ifp,
                &linux_generic_rx_handler, na);
    } else {
        netdev_rx_handler_unregister(ifp);
        return 0;
    }
}


static u16 generic_ndo_select_queue(struct ifnet *ifp, struct mbuf *m)
{
    return skb_get_queue_mapping(m);
}

/* Must be called under rtnl. */
void netmap_catch_packet_steering(struct netmap_adapter *na, int enable)
{
    struct ifnet *ifp = na->ifp;

    if (enable) {
        /*
         * Save the old pointer to the netdev_op
         * create an updated netdev ops replacing the
         * ndo_select_queue function with our custom one,
         * and make the driver use it.
         */
        na->if_transmit = (void *)ifp->netdev_ops;
        *na->generic_ndo_p = *ifp->netdev_ops;  /* Copy */
        na->generic_ndo_p->ndo_select_queue = &generic_ndo_select_queue;
        ifp->netdev_ops = na->generic_ndo_p;
    } else {
	/* Restore the original netdev_ops. */
        ifp->netdev_ops = (void *)na->if_transmit;
    }
}

/* Transmit routine used by generic_netmap_txsync(). Returns 0 on success
   and -1 on error (which may be packet drops or other errors). */
int generic_xmit_frame(struct ifnet *ifp, struct mbuf *m,
	void *addr, u_int len, u_int ring_nr)
{
    netdev_tx_t ret;

    /* Empty the sk_buff. */
    skb_trim(m, 0);

    /* TODO Support the slot flags (NS_FRAG, NS_INDIRECT). */
    skb_copy_to_linear_data(m, addr, len); // skb_store_bits(m, 0, addr, len);
    skb_put(m, len);
    NM_ATOMIC_INC(&m->users);
    m->dev = ifp;
    m->priority = 100;
    skb_set_queue_mapping(m, ring_nr);

    ret = dev_queue_xmit(m);

    if (likely(ret == NET_XMIT_SUCCESS)) {
        return 0;
    }
    if (unlikely(ret != NET_XMIT_DROP)) {
        /* If something goes wrong in the TX path, there is nothing intelligent
           we can do (for now) apart from error reporting. */
        RD(5, "dev_queue_xmit failed: HARD ERROR %d", ret);
    }
    return -1;
}

/* Use ethtool to find the current NIC rings lengths, so that the netmap rings can
   have the same lengths. */
int
generic_find_num_desc(struct ifnet *ifp, unsigned int *tx, unsigned int *rx)
{
    struct ethtool_ringparam rp;

    if (ifp->ethtool_ops && ifp->ethtool_ops->get_ringparam) {
        ifp->ethtool_ops->get_ringparam(ifp, &rp);
        *tx = rp.tx_pending;
        *rx = rp.rx_pending;
    }

    return 0;
}

/* Fills in the output arguments with the number of hardware TX/RX queues. */
void
generic_find_num_queues(struct ifnet *ifp, u_int *txq, u_int *rxq)
{
    *txq = ifp->real_num_tx_queues;
    *rxq = 1; /* TODO ifp->real_num_rx_queues */
}

static int netmap_socket_sendmsg(struct kiocb *iocb, struct socket *sock,
                                 struct msghdr *m, size_t total_len)
{
    D("message_len %d", (int)total_len);
    return total_len;
}

static int netmap_socket_recvmsg(struct kiocb *iocb, struct socket *sock,
                                 struct msghdr *m, size_t total_len, int flags)
{
    D("total_len %d", (int)total_len);

    return 0;
}

static struct proto netmap_socket_proto = {
        .name = "netmap",
        .owner = THIS_MODULE,
        .obj_size = sizeof(struct netmap_socket),
};

static struct proto_ops netmap_socket_ops = {
        .sendmsg = netmap_socket_sendmsg,
        .recvmsg = netmap_socket_recvmsg,
};

static void netmap_sock_write_space(struct sock *sk)
{
    wait_queue_head_t *wqueue;

    if (!sock_writeable(sk) ||
        !test_and_clear_bit(SOCK_ASYNC_NOSPACE, &sk->sk_socket->flags)) {
            return;
    }

    wqueue = sk_sleep(sk);
    if (wqueue && waitqueue_active(wqueue)) {
        wake_up_interruptible_poll(wqueue, POLLOUT | POLLWRNORM | POLLWRBAND);
    }
}

int netmap_sock_setup(struct netmap_priv_d *priv)
{
        struct netmap_socket *np_sock;

        np_sock = priv->np_sock = (struct netmap_socket *)sk_alloc(&init_net, AF_UNSPEC,
                                                        GFP_KERNEL, &netmap_socket_proto);
        if (!np_sock) {
            return ENOMEM;
        }

        np_sock->sock.wq = &np_sock->wq;   /* XXX rcu? */
        init_waitqueue_head(&np_sock->wq.wait);
        np_sock->sock.file = priv->filp;
        np_sock->sock.ops = &netmap_socket_ops;
        sock_init_data(&np_sock->sock, &np_sock->sk);
        np_sock->sk.sk_write_space = &netmap_sock_write_space;

        sock_hold(&np_sock->sk);

        D("socket support OK (%p)", priv);

        return 0;
}

void netmap_sock_teardown(struct netmap_priv_d *priv)
{
    if (priv->np_sock) {
        sock_put(&priv->np_sock->sk);
        /* XXX What?
           kfree(priv->np_sock);
           sk_release_kernel(&priv->np_sock->sk);
           */
        sk_free(&priv->np_sock->sk);
        priv->np_sock = NULL;
        D("socket support freed (%p)", priv);
    }
}


static struct device_driver *
linux_netmap_find_driver(struct device *dev)
{
	struct device_driver *dd;

	while ( (dd = dev->driver) == NULL ) {
		if ( (dev = dev->parent) == NULL )
			return NULL;
	}
	return dd;
}

struct net_device *
ifunit_ref(const char *name)
{
	struct net_device *ifp = dev_get_by_name(&init_net, name);
	struct device_driver *dd;

	if (ifp == NULL)
		return NULL;

	if ( (dd = linux_netmap_find_driver(&ifp->dev)) == NULL )
		goto error;

	if (!try_module_get(dd->owner))
		goto error;

	return ifp;
error:
	dev_put(ifp);
	return NULL;
}

void if_rele(struct net_device *ifp)
{
	struct device_driver *dd;
	dd = linux_netmap_find_driver(&ifp->dev);
	dev_put(ifp);
	if (dd)
		module_put(dd->owner);
}



/*
 * Remap linux arguments into the FreeBSD call.
 * - pwait is the poll table, passed as 'dev';
 *   If pwait == NULL someone else already woke up before. We can report
 *   events but they are filtered upstream.
 *   If pwait != NULL, then pwait->key contains the list of events.
 * - events is computed from pwait as above.
 * - file is passed as 'td';
 */
static u_int
linux_netmap_poll(struct file * file, struct poll_table_struct *pwait)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,28)
	int events = POLLIN | POLLOUT; /* XXX maybe... */
#elif LINUX_VERSION_CODE < KERNEL_VERSION(3,4,0)
	int events = pwait ? pwait->key : POLLIN | POLLOUT;
#else /* in 3.4.0 field 'key' was renamed to '_key' */
	int events = pwait ? pwait->_key : POLLIN | POLLOUT;
#endif
	return netmap_poll((void *)pwait, events, (void *)file);
}


static int
linux_netmap_mmap(struct file *f, struct vm_area_struct *vma)
{
	int error = 0;
	unsigned long off, va;
	vm_ooffset_t pa;
	struct netmap_priv_d *priv = f->private_data;
	/*
	 * vma->vm_start: start of mapping user address space
	 * vma->vm_end: end of the mapping user address space
	 * vma->vm_pfoff: offset of first page in the device
	 */

	// XXX security checks

	error = netmap_get_memory(priv);
	ND("get_memory returned %d", error);
	if (error)
	    return -error;

	if ((vma->vm_start & ~PAGE_MASK) || (vma->vm_end & ~PAGE_MASK)) {
		ND("vm_start = %lx vm_end = %lx", vma->vm_start, vma->vm_end);
		return -EINVAL;
	}

	for (va = vma->vm_start, off = vma->vm_pgoff;
	     va < vma->vm_end;
	     va += PAGE_SIZE, off++)
	{
		pa = netmap_mem_ofstophys(priv->np_mref, off << PAGE_SHIFT);
		if (pa == 0) 
			return -EINVAL;
	
		ND("va %lx pa %p", va, pa);	
		error = remap_pfn_range(vma, va, pa >> PAGE_SHIFT, PAGE_SIZE, vma->vm_page_prot);
		if (error) 
			return error;
	}
	return 0;
}


/*
 * This one is probably already protected by the netif lock XXX
 */
netdev_tx_t
linux_netmap_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	netmap_transmit(dev, skb);
	return (NETDEV_TX_OK);
}


#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,37)	// XXX was 38
#define LIN_IOCTL_NAME	.ioctl
int
linux_netmap_ioctl(struct inode *inode, struct file *file, u_int cmd, u_long data /* arg */)
#else
#define LIN_IOCTL_NAME	.unlocked_ioctl
long
linux_netmap_ioctl(struct file *file, u_int cmd, u_long data /* arg */)
#endif
{
	int ret;
	struct nmreq nmr;
	bzero(&nmr, sizeof(nmr));

        if (cmd == NIOCTXSYNC || cmd == NIOCRXSYNC) {
            data = 0;       /* no argument required here */
        }
	if (data && copy_from_user(&nmr, (void *)data, sizeof(nmr) ) != 0)
		return -EFAULT;
	ret = netmap_ioctl(NULL, cmd, (caddr_t)&nmr, 0, (void *)file);
	if (data && copy_to_user((void*)data, &nmr, sizeof(nmr) ) != 0)
		return -EFAULT;
	return -ret;
}


static int
linux_netmap_release(struct inode *inode, struct file *file)
{
	(void)inode;	/* UNUSED */
	if (file->private_data)
		netmap_dtor(file->private_data);
	return (0);
}


static int
linux_netmap_open(struct inode *inode, struct file *file)
{
	struct netmap_priv_d *priv;
	(void)inode;	/* UNUSED */

	priv = malloc(sizeof(struct netmap_priv_d), M_DEVBUF,
			      M_NOWAIT | M_ZERO);
	if (priv == NULL)
		return -ENOMEM;

	file->private_data = priv;

	return (0);
}


static struct file_operations netmap_fops = {
    .owner = THIS_MODULE,
    .open = linux_netmap_open,
    .mmap = linux_netmap_mmap,
    LIN_IOCTL_NAME = linux_netmap_ioctl,
    .poll = linux_netmap_poll,
    .release = linux_netmap_release,
};


struct socket *get_netmap_socket(int fd)
{
    struct file *filp = fget(fd);
    struct netmap_priv_d *priv;

    if (!filp) {
        return ERR_PTR(EBADF);
    }

    if (filp->f_op != &netmap_fops) {
        return ERR_PTR(EINVAL);
    }

    priv = (struct netmap_priv_d *)filp->private_data;
    if (!priv || !priv->np_sock) {
        return ERR_PTR(EBADF);
    }

    return &priv->np_sock->sock;
}
EXPORT_SYMBOL(get_netmap_socket);


struct miscdevice netmap_cdevsw = { /* same name as FreeBSD */
	MISC_DYNAMIC_MINOR,
	"netmap",
	&netmap_fops,
};


static int linux_netmap_init(void)
{
        /* Errors have negative values on linux. */
	return -netmap_init();
}


static void linux_netmap_fini(void)
{
        netmap_fini();
}


module_init(linux_netmap_init);
module_exit(linux_netmap_fini);

/* export certain symbols to other modules */
EXPORT_SYMBOL(netmap_attach);		/* driver attach routines */
EXPORT_SYMBOL(netmap_detach);		/* driver detach routines */
EXPORT_SYMBOL(netmap_ring_reinit);	/* ring init on error */
EXPORT_SYMBOL(netmap_buffer_lut);
EXPORT_SYMBOL(netmap_total_buffers);	/* index check */
EXPORT_SYMBOL(netmap_buffer_base);
EXPORT_SYMBOL(netmap_reset);		/* ring init routines */
EXPORT_SYMBOL(netmap_buf_size);
EXPORT_SYMBOL(netmap_rx_irq);	        /* default irq handler */
EXPORT_SYMBOL(netmap_no_pendintr);	/* XXX mitigation - should go away */
EXPORT_SYMBOL(netmap_bdg_ctl);		/* bridge configuration routine */
EXPORT_SYMBOL(netmap_bdg_learning);	/* the default lookup function */
EXPORT_SYMBOL(netmap_disable_all_rings);
EXPORT_SYMBOL(netmap_enable_all_rings);


MODULE_AUTHOR("http://info.iet.unipi.it/~luigi/netmap/");
MODULE_DESCRIPTION("The netmap packet I/O framework");
MODULE_LICENSE("Dual BSD/GPL"); /* the code here is all BSD. */
