/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2002 Cluster File Systems, Inc.
 *
 * This code is issued under the GNU General Public License.
 * See the file COPYING in this distribution
 *
 * by Cluster File Systems, Inc.
 */

#define EXPORT_SYMTAB

#include <linux/version.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <asm/unistd.h>

#define DEBUG_SUBSYSTEM S_LDLM

#include <linux/obd_class.h>
#include <linux/lustre_dlm.h>
#include <linux/lustre_net.h>

extern kmem_cache_t *ldlm_resource_slab;
extern kmem_cache_t *ldlm_lock_slab;

static int ldlm_client_callback(struct ldlm_lock *lock, struct ldlm_lock *new,
                                void *data)
{
        LBUG();
        return 0;
}

static int ldlm_enqueue(struct ptlrpc_request *req)
{
        struct ldlm_reply *dlm_rep;
        struct ldlm_request *dlm_req;
        struct lustre_msg *msg, *req_msg;
        ldlm_error_t err;
        int rc;
        int bufsize = sizeof(*dlm_rep);
        
        rc = lustre_pack_msg(1, &bufsize, NULL, &req->rq_replen,
                             &req->rq_repbuf);
        if (rc) {
                CERROR("out of memory\n");
                req->rq_status = -ENOMEM;
                RETURN(0);
        }
        msg = (struct lustre_msg *)req->rq_repbuf;
        req_msg = req->rq_req.lustre;
        dlm_rep = lustre_msg_buf(0, msg);
        dlm_req = lustre_msg_buf(0, req_msg);

        msg->xid = req_msg->xid;

        err = ldlm_local_lock_enqueue(req->rq_obd, dlm_req->ns_id,
                                      &dlm_req->parent_res_handle,
                                      &dlm_req->parent_lock_handle,
                                      dlm_req->res_id, dlm_req->mode,
                                      &dlm_req->flags, ldlm_client_callback,
                                      ldlm_client_callback,
                                      req_msg->buflens[1],
                                      lustre_msg_buf(1, req_msg),
                                      &dlm_rep->lock_handle);
        msg->status = HTON__u32(err);

        /* XXX unfinished */
        return 0;
}

static int ldlm_handle(struct obd_device *dev, struct ptlrpc_service *svc,
                       struct ptlrpc_request *req)
{
        int rc;
        struct ptlreq_hdr *hdr;

        ENTRY;

        hdr = (struct ptlreq_hdr *)req->rq_reqbuf;

        if (NTOH__u32(hdr->type) != PTL_RPC_REQUEST) {
                CERROR("lustre_ldlm: wrong packet type sent %d\n",
                       NTOH__u32(hdr->type));
                rc = -EINVAL;
                GOTO(out, rc);
        }

        rc = lustre_unpack_msg(req->rq_reqbuf, req->rq_reqlen);
        req->rq_reqhdr = (void *)req->rq_reqbuf;
        if (rc) {
                CERROR("lustre_ldlm: Invalid request\n");
                GOTO(out, rc);
        }

        switch (req->rq_reqhdr->opc) {
        case LDLM_ENQUEUE:
                CDEBUG(D_INODE, "enqueue\n");
                OBD_FAIL_RETURN(req, OBD_FAIL_LDLM_ENQUEUE);
                rc = ldlm_enqueue(req);
                break;
#if 0
        case LDLM_CONVERT:
                CDEBUG(D_INODE, "convert\n");
                OBD_FAIL_RETURN(req, OBD_FAIL_LDLM_CONVERT);
                rc = ldlm_convert(req);
                break;

        case LDLM_CANCEL:
                CDEBUG(D_INODE, "cancel\n");
                OBD_FAIL_RETURN(req, OBD_FAIL_LDLM_CANCEL);
                rc = ldlm_cancel(req);
                break;

        case LDLM_CALLBACK:
                CDEBUG(D_INODE, "callback\n");
                OBD_FAIL_RETURN(req, OBD_FAIL_LDLM_CALLBACK);
                rc = ldlm_callback(req);
                break;
#endif

        default:
                rc = ptlrpc_error(dev, svc, req);
                RETURN(rc);
        }

        EXIT;
out:
        if (rc) {
                CERROR("no header\n");
                return 0;
        }

        if( req->rq_status) {
                ptlrpc_error(dev, svc, req);
        } else {
                CDEBUG(D_NET, "sending reply\n");
                ptlrpc_reply(dev, svc, req);
        }

        return 0;
}



static int ldlm_iocontrol(int cmd, struct obd_conn *conn, int len, void *karg,
                          void *uarg)
{
        struct obd_device *obddev = conn->oc_dev;
        int err;

        ENTRY;

        if ( _IOC_TYPE(cmd) != IOC_LDLM_TYPE ||
             _IOC_NR(cmd) < IOC_LDLM_MIN_NR  ||
             _IOC_NR(cmd) > IOC_LDLM_MAX_NR ) {
                CDEBUG(D_IOCTL, "invalid ioctl ( type %d, nr %d, size %d )\n",
                                _IOC_TYPE(cmd), _IOC_NR(cmd), _IOC_SIZE(cmd));
                EXIT;
                return -EINVAL;
        }

        switch (cmd) {
        case IOC_LDLM_TEST: {
                err = ldlm_test(obddev);
                CERROR("-- done err %d\n", err);
                EXIT;
                break;
        }
        default:
                err = -EINVAL;
                EXIT;
                break;
        }

        return err;
}

static int ldlm_setup(struct obd_device *obddev, obd_count len, void *data)
{
        struct ldlm_obd *ldlm = &obddev->u.ldlm;
        int err;
        ENTRY;

        INIT_LIST_HEAD(&obddev->u.ldlm.ldlm_namespaces);
        obddev->u.ldlm.ldlm_lock = SPIN_LOCK_UNLOCKED;

        ldlm->ldlm_service = ptlrpc_init_svc(64 * 1024,
                                             LDLM_REQUEST_PORTAL,
                                             LDLM_REPLY_PORTAL,
                                             "self", ldlm_handle);

        rpc_register_service(ldlm->ldlm_service, "self");

        err = ptlrpc_start_thread(obddev, ldlm->ldlm_service, "lustre_dlm");
        if (err)
                CERROR("cannot start thread\n");

        MOD_INC_USE_COUNT;
        RETURN(0);
}

static int ldlm_cleanup(struct obd_device *obddev)
{
        struct ldlm_obd *ldlm = &obddev->u.ldlm;
        ENTRY;

        ptlrpc_stop_thread(ldlm->ldlm_service);
        rpc_unregister_service(ldlm->ldlm_service);

        if (!list_empty(&ldlm->ldlm_service->srv_reqs)) {
                // XXX reply with errors and clean up
                CERROR("Request list not empty!\n");
        }

        rpc_unregister_service(ldlm->ldlm_service);
        OBD_FREE(ldlm->ldlm_service, sizeof(*ldlm->ldlm_service));

        MOD_DEC_USE_COUNT;
        RETURN(0);
}

struct obd_ops ldlm_obd_ops = {
        o_iocontrol:   ldlm_iocontrol,
        o_setup:       ldlm_setup,
        o_cleanup:     ldlm_cleanup,
        o_connect:     gen_connect,
        o_disconnect:  gen_disconnect
};


static int __init ldlm_init(void)
{
        int rc = obd_register_type(&ldlm_obd_ops, OBD_LDLM_DEVICENAME);
        if (rc != 0)
                return rc;

        ldlm_resource_slab = kmem_cache_create("ldlm_resources",
                                               sizeof(struct ldlm_resource), 0,
                                               SLAB_HWCACHE_ALIGN, NULL, NULL);
        if (ldlm_resource_slab == NULL)
                return -ENOMEM;

        ldlm_lock_slab = kmem_cache_create("ldlm_locks",
                                           sizeof(struct ldlm_lock), 0,
                                           SLAB_HWCACHE_ALIGN, NULL, NULL);
        if (ldlm_lock_slab == NULL) {
                kmem_cache_destroy(ldlm_resource_slab);
                return -ENOMEM;
        }

        return 0;
}

static void __exit ldlm_exit(void)
{
        obd_unregister_type(OBD_LDLM_DEVICENAME);
        kmem_cache_destroy(ldlm_resource_slab);
        kmem_cache_destroy(ldlm_lock_slab);
}

MODULE_AUTHOR("Cluster File Systems, Inc. <braam@clusterfs.com>");
MODULE_DESCRIPTION("Lustre Lock Management Module v0.1");
MODULE_LICENSE("GPL"); 

module_init(ldlm_init);
module_exit(ldlm_exit);
