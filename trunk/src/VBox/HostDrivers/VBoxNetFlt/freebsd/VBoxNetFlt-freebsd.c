/* $Id$ */
/** @file
 * VBoxNetFlt - Network Filter Driver (Host), FreeBSD Specific Code.
 */

/*
 * Copyright (c) 2009 Fredrik Lindberg <fli@shapeshifter.se>
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#include <sys/param.h>
#undef PVM
#include <sys/types.h>
#include <sys/module.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/fcntl.h>
#include <sys/conf.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/syscallsubr.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/ethernet.h>

#include <netgraph/ng_message.h>
#include <netgraph/netgraph.h>
#include <netgraph/ng_parse.h>

#define LOG_GROUP LOG_GROUP_NET_FLT_DRV
#include <VBox/version.h>
#include <VBox/err.h>
#include <VBox/log.h>
#include <iprt/initterm.h>
#include <iprt/string.h>
#include <iprt/spinlock.h>
#include <iprt/process.h>
#include <iprt/assert.h>
#include <iprt/uuid.h>
#include <iprt/alloc.h>
#include <iprt/err.h>

#define VBOXNETFLT_OS_SPECFIC 1
#include "../VBoxNetFltInternal.h"

static int vboxnetflt_modevent(struct module *, int, void *);
static ng_constructor_t    ng_vboxnetflt_constructor;
static ng_rcvmsg_t    ng_vboxnetflt_rcvmsg;
static ng_shutdown_t    ng_vboxnetflt_shutdown;
static ng_newhook_t    ng_vboxnetflt_newhook;
static ng_rcvdata_t    ng_vboxnetflt_rcvdata;
static ng_disconnect_t    ng_vboxnetflt_disconnect;
static int        ng_vboxnetflt_mod_event(module_t mod, int event, void *data);
static int        ng_vboxnetflt_rcv_in(hook_p node, item_p item);
static int        ng_vboxnetflt_rcv_out(hook_p node, item_p item);

/** Netgraph node type */
#define NG_VBOXNETFLT_NODE_TYPE    "vboxnetflt"
/** Netgraph message cookie */
#define NGM_VBOXNETFLT_COOKIE    0x56424f58

/** Input netgraph hook name */
#define NG_VBOXNETFLT_HOOK_IN    "input"
/** Output netgraph hook name */
#define NG_VBOXNETFLT_HOOK_OUT    "output"

/** mbuf tag identifier */
#define MTAG_VBOX        0x56424f58
/** mbuf packet tag */
#define PACKET_TAG_VBOX        128

/*
 * Netgraph command list, we don't support any
 * additional commands.
 */
static const struct ng_cmdlist ng_vboxnetflt_cmdlist[] =
{
    { 0 }
};

/*
 * Netgraph type definition
 */
static struct ng_type ng_vboxnetflt_typestruct =
{
    .version =    NG_ABI_VERSION,
    .name =        NG_VBOXNETFLT_NODE_TYPE,
    .mod_event =    vboxnetflt_modevent,    
    .constructor =    ng_vboxnetflt_constructor,    
    .rcvmsg =     ng_vboxnetflt_rcvmsg,
    .shutdown =    ng_vboxnetflt_shutdown,
    .newhook =    ng_vboxnetflt_newhook,
    .rcvdata =    ng_vboxnetflt_rcvdata,
    .disconnect =     ng_vboxnetflt_disconnect,
    .cmdlist =    ng_vboxnetflt_cmdlist,
};
NETGRAPH_INIT(vboxnetflt, &ng_vboxnetflt_typestruct);
MODULE_VERSION(ng_vboxnetflt, 1);
MODULE_DEPEND(ng_vboxnetflt, vboxdrv, 1, 1, 1);

/**
 * The (common) global data.
 */
static VBOXNETFLTGLOBALS g_VBoxNetFltGlobals;

/**
 * Module event handler, called from netgraph subsystem.
 */
static int vboxnetflt_modevent(struct module *pMod, int enmEventType, void *pvArg)
{
    int rc;

    Log(("VBoxNetFltFreeBSDModuleEvent\n"));

    switch (enmEventType)
    {
        case MOD_LOAD:
            rc = RTR0Init(0);
            if (RT_FAILURE(rc))
            {
                Log(("RTR0Init failed %d\n", rc));
                return RTErrConvertToErrno(rc);
            }

            memset(&g_VBoxNetFltGlobals, 0, sizeof(VBOXNETFLTGLOBALS));
            rc = vboxNetFltInitGlobalsAndIdc(&g_VBoxNetFltGlobals);
            if (RT_FAILURE(rc))
            {
                Log(("vboxNetFltInitGlobalsAndIdc failed %d\n", rc));
                return RTErrConvertToErrno(rc);
            }
            /* No MODULE_VERSION in ng_ether so we can't MODULE_DEPEND it */
            kern_kldload(curthread, "ng_ether", NULL);
            break;

        case MOD_UNLOAD:
            rc = vboxNetFltTryDeleteIdcAndGlobals(&g_VBoxNetFltGlobals);
            memset(&g_VBoxNetFltGlobals, 0, sizeof(VBOXNETFLTGLOBALS));
            RTR0Term();
            break;

        case MOD_SHUTDOWN:
        case MOD_QUIESCE:
        default:
            return EOPNOTSUPP;
    }

    if (RT_SUCCESS(rc))
        return 0;
    return RTErrConvertToErrno(rc);
}

/*
 * Convert from mbufs to vbox scatter-gather data structure
 */
static void vboxNetFltFreeBSDMBufToSG(PVBOXNETFLTINS pThis, struct mbuf *m, PINTNETSG pSG,
    unsigned int cSegs, unsigned int segOffset)
{
    static uint8_t const s_abZero[128] = {0};
    unsigned int i;
    struct mbuf *m0;

    pSG->cbTotal = m_length(m, NULL);
    pSG->pvOwnerData = NULL;
    pSG->pvUserData = NULL;
    pSG->pvUserData2 = NULL;
    pSG->cUsers = 1;
    pSG->fFlags = INTNETSG_FLAGS_TEMP;
    pSG->cSegsAlloc = cSegs;

    for (m0 = m, i = segOffset; m0; m0 = m0->m_next)
    {
        if (m0->m_len == 0)
            continue;

        pSG->aSegs[i].cb = m0->m_len;
        pSG->aSegs[i].pv = mtod(m0, uint8_t *);
        pSG->aSegs[i].Phys = NIL_RTHCPHYS;
        i++;
    }

#ifdef PADD_RUNT_FRAMES_FROM_HOST
    if (pSG->cbTotal < 60)
    {
        pSG->aSegs[i].Phys = NIL_RTHCPHYS;
        pSG->aSegs[i].pv = (void *)&s_abZero[0];
        pSG->aSegs[i].cb = 60 - pSG->cbTotal;
        pSG->cbTotal = 60;
        i++;
    }
#endif
    pSG->cSegsUsed = i;
}

/*
 * Convert to mbufs from vbox scatter-gather data structure
 */
static struct mbuf * vboxNetFltFreeBSDSGMBufFromSG(PVBOXNETFLTINS pThis, PINTNETSG pSG)
{
    struct mbuf *m;
    int error;
    unsigned int i;

    if (pSG->cbTotal == 0 || pSG->aSegs[0].cb == 0)
        return (NULL);

    m = m_getcl(M_WAITOK, MT_DATA, M_PKTHDR);
    if (m == NULL)
        return (NULL);

    m->m_pkthdr.len = m->m_len = 0;
    m->m_pkthdr.rcvif = NULL;

    for (i = 0; i < pSG->cSegsUsed; i++)
    {
        error = m_append(m, pSG->aSegs[i].cb, pSG->aSegs[i].pv);
        if (error == 0)
        {
            m_freem(m);
            return (NULL);
        }
    }
    return (m);
}


static int ng_vboxnetflt_constructor(node_p node)
{
    /* Nothing to do */
    return (EINVAL);
}

/*
 * Setup netgraph hooks
 */
static int ng_vboxnetflt_newhook(node_p node, hook_p hook, const char *name)
{
    PVBOXNETFLTINS pThis = NG_NODE_PRIVATE(node);

    if (strcmp(name, NG_VBOXNETFLT_HOOK_IN) == 0)
    {
#if __FreeBSD_version >= 800000
        NG_HOOK_SET_RCVDATA(hook, ng_vboxnetflt_rcv_in);
        NG_HOOK_SET_TO_INBOUND(hook);
#endif
        pThis->u.s.input = hook;
    }
    else if (strcmp(name, NG_VBOXNETFLT_HOOK_OUT) == 0)
    {
#if __FreeBSD_version >= 800000
        NG_HOOK_SET_RCVDATA(hook, ng_vboxnetflt_rcv_out);
#endif
        pThis->u.s.output = hook;
    }
    else
        return (EINVAL);

    NG_HOOK_HI_STACK(hook);
    return (0);
}

/**
 * Netgraph message processing for node specific messages.
 * We don't accept any special messages so this is not used.
 */
static int ng_vboxnetflt_rcvmsg(node_p node, item_p item, hook_p lasthook)
{
    PVBOXNETFLTINS pThis = NG_NODE_PRIVATE(node);
    struct ng_mesg *msg;
    int error = 0;

    NGI_GET_MSG(item, msg);
    if (msg->header.typecookie != NGM_VBOXNETFLT_COOKIE)
        return (EINVAL);

    switch (msg->header.cmd)
    {
        default:
            error = EINVAL;
    }
    return (error);
}

/**
 * Handle data on netgraph hooks.
 */
static int ng_vboxnetflt_rcvdata(hook_p hook, item_p item)
{
    const node_p node = NG_HOOK_NODE(hook);
    PVBOXNETFLTINS pThis = NG_NODE_PRIVATE(node);
    struct mbuf *m;

    if (pThis->u.s.input == hook)
        return ng_vboxnetflt_rcv_in(hook, item);
    else if (pThis->u.s.output == hook)
        return ng_vboxnetflt_rcv_out(hook, item);
    else
    {
        NGI_GET_M(item, m);
        NG_FREE_ITEM(item);
    }
    return (0);
}

/**
 * Handle incoming hook. This is connected to the
 * input path of the interface, thus handling incoming frames.
 */
static int ng_vboxnetflt_rcv_in(hook_p hook, item_p item)
{
    struct mbuf *m, *m0;
    struct m_tag *mtag;
    const node_p node = NG_HOOK_NODE(hook);
    PVBOXNETFLTINS pThis = NG_NODE_PRIVATE(node);
    struct ifnet *ifp = pThis->u.s.ifp;
    bool fActive, fDropIt = false;
    unsigned int cSegs = 0;
    PINTNETSG pSG;

    NGI_GET_M(item, m);
    NG_FREE_ITEM(item);

    fActive = ASMAtomicUoReadBool(&pThis->fActive);
    if (!fActive)
        goto out;

    mtag = m_tag_locate(m, MTAG_VBOX, PACKET_TAG_VBOX, NULL);
    if (mtag != NULL)
    {
        m_tag_unlink(m, mtag);
        m_tag_free(mtag);
        goto out;
    }
    vboxNetFltRetain(pThis, true /* fBusy */);

    for (m0 = m; m0 != NULL; m0 = m0->m_next)
    {
        if (m0->m_len > 0)
            cSegs++;
    }

#ifdef PADD_RUNT_FRAMES_FROM_HOST
    if (m_length(m, NULL) < 60)
        cSegs++;
#endif

    /* Create a copy of the mbuf and hand it to the virtual switch */
    pSG = RTMemTmpAlloc(RT_OFFSETOF(INTNETSG, aSegs[cSegs]));
    vboxNetFltFreeBSDMBufToSG(pThis, m, pSG, cSegs, 0);
    fDropIt = pThis->pSwitchPort->pfnRecv(pThis->pSwitchPort, pSG, INTNETTRUNKDIR_WIRE);
    RTMemTmpFree(pSG);
    vboxNetFltRelease(pThis, true /* fBusy */);

out:
    /* Only deliver it to the host stack if the destination weren't a guest */
    if (fDropIt)
    {
        m_freem(m);
        return (0);
    }
    ether_demux(ifp, m);
    return (0);
}

/**
 * Handle mbufs on the outgoing hook, frames going to the interface
 */
static int ng_vboxnetflt_rcv_out(hook_p hook, item_p item)
{
    struct mbuf *m, *m0;
    struct m_tag *mtag;
    const node_p node = NG_HOOK_NODE(hook);
    PVBOXNETFLTINS pThis = NG_NODE_PRIVATE(node);
    struct ifnet *ifp = pThis->u.s.ifp;
    unsigned int cSegs = 0;
    bool fDropIt = false, fActive;
    PINTNETSG pSG;

    NGI_GET_M(item, m);
    NG_FREE_ITEM(item);

    fActive = ASMAtomicUoReadBool(&pThis->fActive);
    if (!fActive)
        return ether_output_frame(ifp, m);

    vboxNetFltRetain(pThis, true /* fBusy */);
    /* Pass directly to interface if the packet originated from us */
    mtag = m_tag_locate(m, MTAG_VBOX, PACKET_TAG_VBOX, NULL);
    if (mtag != NULL)
    {
        m_tag_unlink(m, mtag);
        m_tag_free(mtag);
        goto out;
    }

    for (m0 = m; m0 != NULL; m0 = m0->m_next)
    {
        if (m0->m_len > 0)
            cSegs++;
    }

#ifdef PADD_RUNT_FRAMES_FROM_HOST
    if (m_length(m, NULL) < 60)
        cSegs++;
#endif
    /* Create a copy and deliver to the virtual switch */
    pSG = RTMemTmpAlloc(RT_OFFSETOF(INTNETSG, aSegs[cSegs]));
    vboxNetFltFreeBSDMBufToSG(pThis, m, pSG, cSegs, 0);
    fDropIt = pThis->pSwitchPort->pfnRecv(pThis->pSwitchPort, pSG, INTNETTRUNKDIR_HOST);
    RTMemTmpFree(pSG);

out:
    vboxNetFltRelease(pThis, true /* fBusy */);
    if (fDropIt)
    {
        m_freem(m);
        return (0);
    }

    return ether_output_frame(ifp, m);
}

static int ng_vboxnetflt_shutdown(node_p node)
{
    PVBOXNETFLTINS pThis = NG_NODE_PRIVATE(node);
    bool fActive;

    /* Prevent node shutdown if we're active */
    fActive = ASMAtomicUoReadBool(&pThis->fActive);
    if (fActive)
        return (EBUSY);
    NG_NODE_UNREF(node);
    return (0);
}

static int ng_vboxnetflt_disconnect(hook_p hook)
{
    return (0);
}

/**
 * Called to deliver a frame to either the host, the wire or both.
 */
int vboxNetFltPortOsXmit(PVBOXNETFLTINS pThis, PINTNETSG pSG, uint32_t fDst)
{
    void (*input_f)(struct ifnet *, struct mbuf *);
    struct ifnet *ifp;
    struct mbuf *m;
    struct m_tag *mtag;
    bool fActive;
    int error;

    ifp = (void *)ASMAtomicUoReadPtr((void * volatile *)&pThis->u.s.ifp);

    if (fDst & INTNETTRUNKDIR_WIRE)
    {
        m = vboxNetFltFreeBSDSGMBufFromSG(pThis, pSG);
        if (m == NULL)
            return VERR_NO_MEMORY;
        m = m_pullup(m, ETHER_HDR_LEN);
        if (m == NULL)
            return VERR_NO_MEMORY;

        m->m_flags |= M_PKTHDR;
        ether_output_frame(ifp, m);
    }

    if (fDst & INTNETTRUNKDIR_HOST)
    {
        m = vboxNetFltFreeBSDSGMBufFromSG(pThis, pSG);
        if (m == NULL)
            return VERR_NO_MEMORY;
        m = m_pullup(m, ETHER_HDR_LEN);
        if (m == NULL)
            return VERR_NO_MEMORY;
        /*
         * Delivering packets to the host will be captured by the
         * input hook. Tag the packet with a mbuf tag so that we
         * can skip re-delivery of the packet to the guest during
         * input hook processing.
         */
        mtag = m_tag_alloc(MTAG_VBOX, PACKET_TAG_VBOX, 0, M_NOWAIT);
        if (mtag == NULL)
        {
            m_freem(m);
            return VERR_NO_MEMORY;
        }

        m_tag_init(m);
        m_tag_prepend(m, mtag);
        m->m_flags |= M_PKTHDR;
        m->m_pkthdr.rcvif = ifp;
        ifp->if_input(ifp, m);
    }
    return VINF_SUCCESS;
}

int vboxNetFltOsInitInstance(PVBOXNETFLTINS pThis, void *pvContext)
{
    char nam[NG_NODESIZ];
    struct ifnet *ifp;
    node_p node;
    RTSPINLOCKTMP Tmp = RTSPINLOCKTMP_INITIALIZER;

    NOREF(pvContext);
    ifp = ifunit(pThis->szName);
    if (ifp == NULL)
        return VERR_INTNET_FLT_IF_NOT_FOUND;

    /* Create a new netgraph node for this instance */
    if (ng_make_node_common(&ng_vboxnetflt_typestruct, &node) != 0)
        return VERR_INTERNAL_ERROR;        

    RTSpinlockAcquire(pThis->hSpinlock, &Tmp);
    ASMAtomicUoWritePtr((void * volatile *)&pThis->u.s.ifp, ifp);
    pThis->u.s.node = node;
    bcopy(IF_LLADDR(ifp), &pThis->u.s.Mac, ETHER_ADDR_LEN);
    ASMAtomicUoWriteBool(&pThis->fDisconnectedFromHost, false);
    RTSpinlockRelease(pThis->hSpinlock, &Tmp);

    NG_NODE_SET_PRIVATE(node, pThis);
    /* Attempt to name it vboxnetflt_<ifname> */
    snprintf(nam, NG_NODESIZ, "vboxnetflt_%s", pThis->szName);
    ng_name_node(node, nam);

    return VINF_SUCCESS;
}

bool vboxNetFltOsMaybeRediscovered(PVBOXNETFLTINS pThis)
{
    struct ifnet *ifp, *ifp0;

    ifp = (struct ifnet *)ASMAtomicUoReadPtr((void * volatile *)&pThis->u.s.ifp);
    /*
     * Attempt to check if the interface is still there and re-initialize if
     * something has changed.
     */
    ifp0 = ifunit(pThis->szName);
    if (ifp != ifp0)
    {
        ASMAtomicUoWriteBool(&pThis->fDisconnectedFromHost, true);
        ng_rmnode_self(pThis->u.s.node);
        pThis->u.s.node = NULL;
    }

    if (ifp0 != NULL)
        vboxNetFltOsInitInstance(pThis, NULL);

    return !ASMAtomicUoReadBool(&pThis->fDisconnectedFromHost);
}

void vboxNetFltOsDeleteInstance(PVBOXNETFLTINS pThis)
{

    if (pThis->u.s.node != NULL)
        ng_rmnode_self(pThis->u.s.node);
    pThis->u.s.node = NULL;
}

int vboxNetFltOsPreInitInstance(PVBOXNETFLTINS pThis)
{

    pThis->u.s.ifp = NULL;
    pThis->u.s.flags = 0;
    pThis->u.s.node = NULL;
    return VINF_SUCCESS;
}

void vboxNetFltPortOsSetActive(PVBOXNETFLTINS pThis, bool fActive)
{
    struct ifnet *ifp;
    struct ifreq ifreq;
    int error;
    node_p node;
    struct ng_mesg *msg;
    struct ngm_connect *con;
    struct ngm_rmhook *rm;
    char path[NG_PATHSIZ];

    Log(("%s: fActive:%d\n", __func__, fActive));

    ifp = (struct ifnet *)ASMAtomicUoReadPtr((void * volatile *)&pThis->u.s.ifp);
    node = (node_p)ASMAtomicUoReadPtr((void * volatile *)&pThis->u.s.node);

    memset(&ifreq, 0, sizeof(struct ifreq));
    /* Activate interface */
    if (fActive)
    {
        pThis->u.s.flags = ifp->if_flags;
        ifpromisc(ifp, 1);

        /* ng_ether nodes are named after the interface name */
        snprintf(path, sizeof(path), "%s:", ifp->if_xname);

        /*
         * Send a netgraph connect message to the ng_ether node
         * assigned to the bridged interface. Connecting
         * the hooks 'lower' (ng_ether) to out 'input'.
         */
        NG_MKMESSAGE(msg, NGM_GENERIC_COOKIE, NGM_CONNECT,
            sizeof(struct ngm_connect), M_NOWAIT);
        if (msg == NULL)
            return;
        con = (struct ngm_connect *)msg->data;
        snprintf(con->path, NG_PATHSIZ, "vboxnetflt_%s:", ifp->if_xname);
        strlcpy(con->ourhook, "lower", NG_HOOKSIZ);
        strlcpy(con->peerhook, "input", NG_HOOKSIZ);
        NG_SEND_MSG_PATH(error, node, msg, path, 0);

        /*
         * Do the same for the hooks 'upper' (ng_ether) and our
         * 'output' hook.
         */
        NG_MKMESSAGE(msg, NGM_GENERIC_COOKIE, NGM_CONNECT,
            sizeof(struct ngm_connect), M_NOWAIT);
        if (msg == NULL)
            return;
        con = (struct ngm_connect *)msg->data;
        snprintf(con->path, NG_PATHSIZ, "vboxnetflt_%s:",
            ifp->if_xname);
        strlcpy(con->ourhook, "upper", sizeof(con->ourhook));
        strlcpy(con->peerhook, "output", sizeof(con->peerhook));
        NG_SEND_MSG_PATH(error, node, msg, path, 0);
    }
    else
    {
        /* De-activate interface */
        pThis->u.s.flags = 0;
        ifpromisc(ifp, 0);

        /* Disconnect msgs are addressed to ourself */
        snprintf(path, sizeof(path), "vboxnetflt_%s:", ifp->if_xname);

        /*
         * Send a netgraph message to disconnect our 'input' hook
         */
        NG_MKMESSAGE(msg, NGM_GENERIC_COOKIE, NGM_RMHOOK,
            sizeof(struct ngm_rmhook), M_NOWAIT);
        if (msg == NULL)
            return;
        rm = (struct ngm_rmhook *)msg->data;
        strlcpy(rm->ourhook, "input", NG_HOOKSIZ);
        NG_SEND_MSG_PATH(error, node, msg, path, 0);

        /*
         * Send a netgraph message to disconnect our 'output' hook
         */
        NG_MKMESSAGE(msg, NGM_GENERIC_COOKIE, NGM_RMHOOK,
            sizeof(struct ngm_rmhook), M_NOWAIT);
        if (msg == NULL)
            return;
        rm = (struct ngm_rmhook *)msg->data;
        strlcpy(rm->ourhook, "output", NG_HOOKSIZ);
        NG_SEND_MSG_PATH(error, node, msg, path, 0);
    }
}

bool vboxNetFltPortOsIsPromiscuous(PVBOXNETFLTINS pThis)
{
    return (pThis->u.s.flags & IFF_PROMISC) ? true : false;
}

void vboxNetFltPortOsGetMacAddress(PVBOXNETFLTINS pThis, PRTMAC pMac)
{
    *pMac = pThis->u.s.Mac;
}

bool vboxNetFltPortOsIsHostMac(PVBOXNETFLTINS pThis, PCRTMAC pMac)
{
    return pThis->u.s.Mac.au16[0] == pMac->au16[0]
        && pThis->u.s.Mac.au16[1] == pMac->au16[1]
        && pThis->u.s.Mac.au16[2] == pMac->au16[2];
}

int vboxNetFltOsDisconnectIt(PVBOXNETFLTINS pThis)
{
    return VINF_SUCCESS;
}

int vboxNetFltOsConnectIt(PVBOXNETFLTINS pThis)
{
    return VINF_SUCCESS;
}
