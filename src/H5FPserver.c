/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by the Board of Trustees of the University of Illinois.         *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of HDF5.  The full HDF5 copyright notice, including     *
 * terms governing use, modification, and redistribution, is contained in    *
 * the files COPYING and Copyright.html.  COPYING can be found at the root   *
 * of the source code distribution tree; Copyright.html can be found at the  *
 * root level of an installed copy of the electronic HDF5 document set and   *
 * is linked from the top-level documents page.  It can also be found at     *
 * http://hdf.ncsa.uiuc.edu/HDF5/doc/Copyright.html.  If you do not have     *
 * access to either file, you may request a copy from hdfhelp@ncsa.uiuc.edu. *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * NOTE! This is NOT thread safe!
 *
 * NOTE: There will be caveats on call-back functions.
 */

/*
 * Purpose:
 *
 *      This file has all of the code that a server (SAP) would run to
 *      handle requests from clients.
 */

#include "H5private.h"          /* Generic Functions                    */
#include "H5ACprivate.h"        /* Metadata Cache                       */
#include "H5Eprivate.h"         /* Error Handling                       */
#include "H5Oprivate.h"         /* Object Headers                       */
#include "H5TBprivate.h"        /* Threaded, Balanced, Binary Trees     */

#ifdef H5_HAVE_FPHDF5

#include "H5FPprivate.h"        /* Flexible Parallel Functions          */

/* Pablo mask */
#define PABLO_MASK      H5FPserver_mask

/* Is the interface initialized? */
static int interface_initialize_g = 0;
#define INTERFACE_INIT  NULL

/* Internal SAP structures */

typedef struct {
    uint8_t         oid[H5R_OBJ_REF_BUF_SIZE]; /* Buffer to store OID of object */
    unsigned        owned_rank; /* rank which has the lock                  */
    H5FP_obj_t      obj_type;   /* type of object being locked              */
    unsigned        ref_count;  /* number of times lock was aquired by proc */
    H5FP_lock_t     rw_lock;    /* indicates if it's a read or write lock   */
} H5FP_object_lock;

typedef struct {
    H5FD_mem_t      mem_type;   /* type of memory updated, if req'd         */
    H5AC_subid_t    type_id;    /* id of the type of this metadata          */
    H5FP_obj_t      obj_type;   /* type of object modified                  */
    haddr_t         addr;       /* address of the metadata                  */
    unsigned        md_size;    /* size of the metadata                     */
    char           *metadata;   /* encoded metadata about the object        */
} H5FP_mdata_mod;

typedef struct {
    unsigned        file_id;    /* the file id the SAP keeps per file       */
    char           *filename;   /* the filename - of dubious use            */
    int             closing;    /* we're closing the file - no more changes */
    unsigned        num_mods;   /* number of mdata writes outstanding       */
    H5TB_TREE      *mod_tree;   /* a tree of metadata updates done          */
    H5TB_TREE      *locks;      /* a tree of locks on objects in the file   */
} H5FP_file_info;

/*
 * This marks the point at which we want to dump all of the metadata
 * write to a process so that that process can write them to the file.
 */
#define H5FP_MDATA_CACHE_HIGHWATER_MARK     1024

static H5TB_TREE *file_info_tree;

/* local functions */
static herr_t H5FP_sap_receive(H5FP_request *req, int source, int tag, char **buf);

    /* local functions to generate unique ids for messages */
static unsigned H5FP_gen_sap_file_id(void);

    /* local functions for handling object locks */
static int H5FP_object_lock_cmp(H5FP_object_lock *o1,
                                H5FP_object_lock *o2,
                                int cmparg);
static H5FP_object_lock *H5FP_new_object_lock(const unsigned char *oid,
                                              unsigned rank,
                                              H5FP_obj_t obj_type,
                                              H5FP_lock_t rw_lock);
static herr_t H5FP_free_object_lock(H5FP_object_lock *ol);
static H5FP_object_lock *H5FP_find_object_lock(H5FP_file_info *info,
                                               unsigned char *oid);
static herr_t H5FP_remove_object_lock_from_list(H5FP_file_info *info,
                                                H5FP_object_lock *ol);

    /* local file information handling functions */
static herr_t H5FP_add_new_file_info_to_list(unsigned file_id, char *filename);
static int H5FP_file_info_cmp(H5FP_file_info *k1, H5FP_file_info *k2, int cmparg);
static H5FP_file_info *H5FP_new_file_info_node(unsigned file_id, char *filename);
static H5FP_file_info *H5FP_find_file_info(unsigned file_id);
static herr_t H5FP_remove_file_id_from_list(unsigned file_id);
static herr_t H5FP_free_file_info_node(H5FP_file_info *info);

    /* local file modification structure handling functions */
static H5FP_mdata_mod *H5FP_new_file_mod_node(unsigned rank,
                                              H5FD_mem_t mem_type,
                                              H5AC_subid_t type_id,
                                              haddr_t addr,
                                              unsigned md_size,
                                              char *metadata);
static herr_t H5FP_free_mod_node(H5FP_mdata_mod *info);

    /* local request handling functions */
static herr_t H5FP_sap_handle_open_request(H5FP_request req,
                                           char *mdata,
                                           unsigned md_size);
static herr_t H5FP_sap_handle_lock_request(H5FP_request req);
static herr_t H5FP_sap_handle_release_lock_request(H5FP_request req);
static herr_t H5FP_sap_handle_write_request(H5FP_request req,
                                            char *mdata,
                                            unsigned md_size);
static herr_t H5FP_sap_handle_read_request(H5FP_request req);
static herr_t H5FP_sap_handle_close_request(H5FP_request req);

/*
 *===----------------------------------------------------------------------===
 *                    Public Library (non-API) Functions
 *===----------------------------------------------------------------------===
 */

/*
 * Function:    H5FP_sap_receive_loop
 * Purpose:     Just receive message after message from the other
 *              processes and process that message. Return when we
 *              receive an "H5FP_REQ_STOP" message from all processes in
 *              H5FP_SAP_COMM.
 * Return:      Success:    SUCCEED
 *              Failure:    FAIL
 * Programmer:  Bill Wendling, 05. August, 2002
 * Modifications:
 */
herr_t
H5FP_sap_receive_loop(void)
{
    herr_t ret_value = SUCCEED;
    int stop = 0;

    FUNC_ENTER_NOAPI(H5FP_sap_receive_loop, FAIL);

    /* Create the file structure tree. */
    if ((file_info_tree = H5TB_dmake((H5TB_cmp_t)H5FP_file_info_cmp,
                                     sizeof(H5FP_file_info), FALSE)) == NULL)
        HGOTO_ERROR(H5E_FPHDF5, H5E_CANTMAKETREE, FAIL, "cannot make TBBT tree");

    for (;;) {
        H5FP_request req; 
        char *buf = NULL;
        herr_t hrc;

        if (H5FP_sap_receive(&req, MPI_ANY_SOURCE, H5FP_TAG_REQUEST, &buf) != SUCCEED)
            HGOTO_ERROR(H5E_FPHDF5, H5E_CANTRECV, FAIL, "cannot receive messages");

        switch (req.req_type) {
        case H5FP_REQ_OPEN:
            if ((hrc = H5FP_sap_handle_open_request(req, buf, req.md_size)) != SUCCEED)
                HGOTO_ERROR(H5E_FPHDF5, H5E_CANTOPENOBJ, FAIL, "cannot open file");
            break;
        case H5FP_REQ_LOCK:
        case H5FP_REQ_LOCK_END:
            hrc = H5FP_sap_handle_lock_request(req);
            break;
        case H5FP_REQ_RELEASE:
        case H5FP_REQ_RELEASE_END:
            hrc = H5FP_sap_handle_release_lock_request(req);
            break;
        case H5FP_REQ_WRITE:
            hrc = H5FP_sap_handle_write_request(req, buf, req.md_size);
            break;
        case H5FP_REQ_READ:
            hrc = H5FP_sap_handle_read_request(req);
            break;
        case H5FP_REQ_CLOSE:
            hrc = H5FP_sap_handle_close_request(req);
            break;
        case H5FP_REQ_STOP:
            if (++stop == H5FP_comm_size - 1)
                goto done;
            break;
        default:
            HGOTO_ERROR(H5E_FPHDF5, H5E_ARGS, FAIL, "invalid request type");
        }

        /*
         * If the above calls didn't succeed, free the buffer
         */
        if (hrc != SUCCEED)
            HDfree(buf);
    }

done:
    FUNC_LEAVE_NOAPI(ret_value);
}

/*
 * Function:    H5FP_sap_receive
 * Purpose:     Receive a message from SOURCE with the given TAG. The REQ
 *              object passed in as a pointer is filled by the MPI_Recv
 *              function.
 * Return:      Success:    Pointer to string passed in, if one was sent.
 *                          As well as the SAP_request object.
 *              Failure:    FAIL
 * Programmer:  Bill Wendling, 17. September, 2002
 * Modifications:
 */
static herr_t
H5FP_sap_receive(H5FP_request *req, int source, int tag, char **buf)
{
    MPI_Status status;
    herr_t ret_value = SUCCEED;
    int mrc;

    FUNC_ENTER_NOINIT(H5FP_sap_receive);

    HDmemset(&status, 0, sizeof(status));

    if ((mrc = MPI_Recv(req, 1, H5FP_request_t, source, tag,
                        H5FP_SAP_COMM, &status)) != MPI_SUCCESS)
        HMPI_GOTO_ERROR(FAIL, "MPI_Recv failed", mrc);

    if (buf && req->md_size)
        if (H5FP_read_metadata(buf, (int)req->md_size, (int)req->proc_rank) == FAIL)
            HGOTO_ERROR(H5E_FPHDF5, H5E_CANTRECV, FAIL, "can't read metadata from process");

done:
    FUNC_LEAVE_NOAPI(ret_value);
}

/*
 * Function:    H5FP_object_lock_cmp
 * Purpose:     Comparison function for the TBBT.
 * Return:      <0, 0, or >0
 * Programmer:  Bill Wendling, 09. September 2002
 * Modifications:
 */
static int
H5FP_object_lock_cmp(H5FP_object_lock *o1,
                     H5FP_object_lock *o2,
                     int UNUSED cmparg)
{
    FUNC_ENTER_NOINIT(H5FP_object_lock_cmp);
    assert(o1);
    assert(o2);
    FUNC_LEAVE_NOAPI(HDmemcmp(o1->oid, o2->oid, sizeof(o1->oid)));
}

/*
 * Function:    H5FP_new_object_lock
 * Purpose:     Create a new object lock. The locks are keyed off of the
 *              OID when they're inserted into the TBBT. There's a
 *              reference count so the same process can request the lock
 *              multiple times, if need be. The rank of the requesting
 *              process is kept around so that we can determine who
 *              wanted it in the first place. RW_LOCK tells us what kind
 *              of lock it is -- READ or WRITE.
 * Return:      Success:    Pointer to SAP_OBJ_LOCK structure.
 *              Failure:    NULL
 * Programmer:  Bill Wendling, 09. September 2002
 * Modifications:
 */
static H5FP_object_lock *
H5FP_new_object_lock(const unsigned char *oid, unsigned rank,
                     H5FP_obj_t obj_type, H5FP_lock_t rw_lock)
{
    H5FP_object_lock *ret_value = NULL;
    
    FUNC_ENTER_NOINIT(H5FP_new_object_lock);
    assert(oid);

    if ((ret_value = (H5FP_object_lock *)HDmalloc(sizeof(H5FP_object_lock))) == NULL)
        HGOTO_ERROR(H5E_RESOURCE, H5E_NOSPACE, NULL, "out of memory");

    H5FP_COPY_OID(ret_value->oid, oid);
    ret_value->owned_rank = rank;
    ret_value->obj_type = obj_type;
    ret_value->ref_count = 1;
    ret_value->rw_lock = rw_lock;

done:
    FUNC_LEAVE_NOAPI(ret_value);
}

/*
 * Function:    H5FP_free_object_lock
 * Purpose:     Free up the space allocated for the object lock.
 * Return:      SUCCEED (never fails)
 * Programmer:  Bill Wendling, 09. September 2002
 * Modifications:
 */
static herr_t
H5FP_free_object_lock(H5FP_object_lock *ol)
{
    FUNC_ENTER_NOINIT(H5FP_free_object_lock);
    HDfree(ol);
    FUNC_LEAVE_NOAPI(SUCCEED);
}

/*
 * Function:    H5FP_find_object_lock
 * Purpose:     Find the object lock for the given OID if there is one.
 * Return:      Success:    Pointer to the object
 *              Failure:    NULL
 * Programmer:  Bill Wendling, 09. September 2002
 * Modifications:
 */
static H5FP_object_lock *
H5FP_find_object_lock(H5FP_file_info *info, unsigned char *oid)
{
    H5FP_object_lock *ret_value = NULL;

    FUNC_ENTER_NOINIT(H5FP_find_object_lock);

    assert(info);
    assert(oid);

    if (info->locks && info->locks->root) {
        H5TB_NODE *node;
        H5FP_object_lock ol;

        H5FP_COPY_OID(ol.oid, oid); /* This is the key field for the TBBT */

        if ((node = H5TB_dfind(info->locks, (void *)&ol, NULL)) == NULL)
            HGOTO_ERROR(H5E_FPHDF5, H5E_NOTFOUND, NULL, "lock not found");

        ret_value = (H5FP_object_lock *)node->data;
    }

done:
    FUNC_LEAVE_NOAPI(ret_value);
}

/*
 * Function:    H5FP_remove_object_lock_from_list
 * Purpose:     Remove the object lock from the file structure's lock
 *              list.
 * Return:      Success:    SUCCEED
 *              Failure:    FAIL
 * Programmer:  Bill Wendling, 09. September 2002
 * Modifications:
 */
static herr_t
H5FP_remove_object_lock_from_list(H5FP_file_info *info,
                                  H5FP_object_lock *ol)
{
    H5TB_NODE *node;
    herr_t ret_value = FAIL;

    FUNC_ENTER_NOINIT(H5FP_remove_object_lock_from_list);

    if ((node = H5TB_dfind(info->locks, (void *)ol, NULL)) != NULL) {
        H5FP_free_object_lock(H5TB_rem(&info->locks->root, node, NULL));
        ret_value = SUCCEED;
    }

    FUNC_LEAVE_NOAPI(ret_value);
}

/*
 * Function:    H5FP_file_mod_cmp
 * Purpose:     Comparison function for the TBBT.
 * Return:      <0, 0, or >0
 * Programmer:  Bill Wendling, 27. August, 2002
 * Modifications:
 */
static int
H5FP_file_mod_cmp(H5FP_mdata_mod *k1,
                  H5FP_mdata_mod *k2,
                  int UNUSED cmparg)
{
    FUNC_ENTER_NOINIT(H5FP_file_mod_cmp);
    assert(k1);
    assert(k2);
    FUNC_LEAVE_NOAPI(k1->addr - k2->addr);
}

/*
 * Function:    H5FP_free_mod_node
 * Purpose:     Helper function to free up an SAP_FILE_MOD node and all
 *              of the malloced space it has.
 * Return:      SUCCEED (doesn't fail)
 * Programmer:  Bill Wendling, 31. July, 2002
 * Modifications:
 */
static herr_t
H5FP_free_mod_node(H5FP_mdata_mod *info)
{
    FUNC_ENTER_NOINIT(H5FP_free_mod_node);

    if (info) {
        HDfree(info->metadata);
        HDfree(info);
    }

    FUNC_LEAVE_NOAPI(SUCCEED);
}

/*
 * Function:    H5FP_new_file_mod_node
 * Purpose:     Create a new sap_file_mod node and initialize it. This
 *              object now has responsibility for freeing the metadata
 *              information.
 * Return:      Success:    Pointer to new sap_file_mod structure.
 *              Failure:    NULL
 * Programmer:  Bill Wendling, 02. August, 2002
 * Modifications:
 */
static H5FP_mdata_mod *
H5FP_new_file_mod_node(unsigned UNUSED rank, H5FD_mem_t mem_type,
                       H5AC_subid_t type_id, haddr_t addr, unsigned md_size,
                       char *metadata)
{
    H5FP_mdata_mod *ret_value = NULL;

    FUNC_ENTER_NOINIT(H5FP_new_file_mod_node);

    if ((ret_value = (H5FP_mdata_mod *)HDmalloc(sizeof(H5FP_mdata_mod))) == NULL)
        HGOTO_ERROR(H5E_RESOURCE, H5E_NOSPACE, NULL, "out of memory");

    ret_value->mem_type = mem_type;
    ret_value->addr = addr;
    ret_value->type_id = type_id;
    ret_value->md_size = md_size;
    ret_value->metadata = metadata;

done:
    FUNC_LEAVE_NOAPI(ret_value);
}

/*
 * Function:    H5FP_add_file_mod_to_list
 * Purpose:     Add a metadata write to a file ID. If the metadata is
 *              already in the cache, then we just replace it with the
 *              updated bits. (Only the metadata info and size should
 *              change in this case.)
 * Return:      Success:    SUCCEED
 *              Failure:    FAIL
 * Programmer:  Bill Wendling, 02. August, 2002
 * Modifications:
 */
static herr_t
H5FP_add_file_mod_to_list(H5FP_file_info *info, H5FD_mem_t mem_type, 
                          H5AC_subid_t type_id, haddr_t addr, unsigned rank,
                          unsigned md_size, char *metadata)
{
    H5FP_mdata_mod *fm, mod;
    H5TB_NODE *node;
    herr_t ret_value = FAIL;

    FUNC_ENTER_NOINIT(H5FP_add_file_mod_to_list);

    assert(info);

    mod.addr = addr;    /* This is the key field for the TBBT */

    if ((node = H5TB_dfind(info->mod_tree, (void *)&mod, NULL)) != NULL) {
        /*
         * The metadata is in the cache already. All we have to do is
         * replace what's there. The addr and type should be the same.
         * The only things to change is the metadata and its size.
         */
        fm = (H5FP_mdata_mod *)node->data;
        HDfree(fm->metadata);
        fm->metadata = metadata;
        fm->md_size = md_size;
        ret_value = SUCCEED;
    } else if ((fm = H5FP_new_file_mod_node(rank, mem_type, type_id, addr,
                                            md_size, metadata)) != NULL) {
        if (!H5TB_dins(info->mod_tree, (void *)fm, NULL))
            HGOTO_ERROR(H5E_FPHDF5, H5E_CANTINSERT, FAIL,
                        "can't insert modification into tree");

        ret_value = SUCCEED;
    }

done:
    FUNC_LEAVE_NOAPI(ret_value);
}

/*
 * Function:    H5FP_free_file_info_node
 * Purpose:     Helper function to free up an SAP_FILE_STRUCT node and all
 *              of the malloced space it has.
 * Return:      SUCCEED (never fails)
 * Programmer:  Bill Wendling, 31. July, 2002
 * Modifications:
 */
static herr_t
H5FP_free_file_info_node(H5FP_file_info *info)
{
    FUNC_ENTER_NOINIT(H5FP_free_file_info_node);

    if (info) {
        H5TB_dfree(info->mod_tree, (void (*)(void*))H5FP_free_mod_node, NULL);
        H5TB_dfree(info->locks, (void (*)(void*))H5FP_free_object_lock, NULL);
        HDfree(info->filename);
        HDfree(info);
    }

    FUNC_LEAVE_NOAPI(SUCCEED);
}

/*
 * Function:    H5FP_file_info_cmp
 * Purpose:     Compare two sap_file_info structs for the TBBT.
 * Return:      <0, 0, >0
 * Programmer:  Bill Wendling, 27. August, 2002
 * Modifications:
 */
static int
H5FP_file_info_cmp(H5FP_file_info *k1, H5FP_file_info *k2, int UNUSED cmparg)
{
    FUNC_ENTER_NOINIT(H5FP_file_info_cmp);
    assert(k1);
    assert(k2);
    FUNC_LEAVE_NOAPI(k1->file_id - k2->file_id);
}

/*
 * Function:    H5FP_new_file_info_node
 * Purpose:     Create and initialize an sap_file_info node.
 * Return:      Success: Pointer to new node
 *              Failure: NULL
 * Programmer:  Bill Wendling, 02. August, 2002
 * Modifications:
 */
static H5FP_file_info *
H5FP_new_file_info_node(unsigned file_id, char *filename)
{
    H5FP_file_info *ret_value;

    FUNC_ENTER_NOINIT(H5FP_new_file_info_node);

    if ((ret_value = (H5FP_file_info *)HDmalloc(sizeof(H5FP_file_info))) == NULL)
        HGOTO_ERROR(H5E_RESOURCE, H5E_NOSPACE, NULL, "out of memory");

    ret_value->file_id = file_id;
    ret_value->filename = filename;
    ret_value->closing = FALSE;
    ret_value->num_mods = 0;

    if ((ret_value->mod_tree = H5TB_dmake((H5TB_cmp_t)H5FP_file_mod_cmp,
                                          sizeof(H5FP_mdata_mod), FALSE)) == NULL) {
        HDfree(ret_value);
        HGOTO_ERROR(H5E_FPHDF5, H5E_CANTMAKETREE, NULL, "cannot make TBBT tree");
    }

    if ((ret_value->locks = H5TB_dmake((H5TB_cmp_t)H5FP_object_lock_cmp,
                                       sizeof(H5FP_object_lock), FALSE)) == NULL) {
        H5TB_dfree(ret_value->mod_tree, (void (*)(void*))H5FP_free_mod_node, NULL);
        HDfree(ret_value);
        HGOTO_ERROR(H5E_FPHDF5, H5E_CANTMAKETREE, NULL, "cannot make TBBT tree");
    }

done:
    FUNC_LEAVE_NOAPI(ret_value);
}

/*
 * Function:    H5FP_find_file_info
 * Purpose:     Find the file structure for the requested file_id.
 * Return:      Success:    Pointer to the file structure
 *              Failure:    NULL
 * Programmer:  Bill Wendling, 31. July, 2002
 * Modifications:
 */
static H5FP_file_info *
H5FP_find_file_info(unsigned file_id)
{
    H5TB_NODE *node = NULL;
    H5FP_file_info *ret_value = NULL;

    FUNC_ENTER_NOINIT(H5FP_find_file_info);

    if (file_info_tree && file_info_tree->root) {
        H5FP_file_info s;

        s.file_id = file_id;    /* This is the key field for the TBBT */

        if ((node = H5TB_dfind(file_info_tree, (void *)&s, NULL)) != NULL)
            ret_value = (H5FP_file_info *)node->data;
    }

    FUNC_LEAVE_NOAPI(ret_value);
}

/*
 * Function:    H5FP_add_new_file_info_to_list
 * Purpose:     Add a FILE_ID to the list of file IDS.
 * Return:      SUCCEED if the node was added
 *              FAIL otherwise
 * Programmer:  Bill Wendling, 02. August, 2002
 * Modifications:
 */
static herr_t
H5FP_add_new_file_info_to_list(unsigned file_id, char *filename)
{
    H5FP_file_info *info;
    herr_t ret_value = FAIL;

    FUNC_ENTER_NOINIT(H5FP_add_new_file_info_to_list);

    if ((info = H5FP_new_file_info_node(file_id, filename)) != NULL) {
        if (!H5TB_dins(file_info_tree, (void *)info, NULL)) {
            H5FP_free_file_info_node(info);
            HGOTO_ERROR(H5E_FPHDF5, H5E_CANTINSERT, FAIL,
                        "can't insert file structure into tree");
        }

        ret_value = SUCCEED;
    }

done:
    FUNC_LEAVE_NOAPI(ret_value);
}

/*
 * Function:    H5FP_remove_file_id_from_list
 * Purpose:     Remove an FILE_ID from the list of file IDS.
 *
 *              NOTE: This should only be called after all modifications to
 *              the file descriptor have been synced to all processes and the
 *              file has been closed by all processes.
 *
 * Return:      Success:    SUCCEED
 *              Failure:    FAIL
 * Programmer:  Bill Wendling, 31. July, 2002
 * Modifications:
 */
static herr_t
H5FP_remove_file_id_from_list(unsigned file_id)
{
    H5FP_file_info s;
    H5TB_NODE *node;
    herr_t ret_value = FAIL;

    FUNC_ENTER_NOINIT(H5FP_remove_file_id_from_list);

    s.file_id = file_id;    /* This is the key field for the TBBT */

    if ((node = H5TB_dfind(file_info_tree, (void *)&s, NULL)) != NULL) {
        H5FP_free_file_info_node(H5TB_rem(&file_info_tree->root, node, NULL));
        ret_value = SUCCEED;
    }

    FUNC_LEAVE_NOAPI(ret_value);
}

/*
 *===----------------------------------------------------------------------===
 *                     Functions to reply to requests
 *===----------------------------------------------------------------------===
 */

/*
 * Function:    H5FP_send_reply
 * Purpose:     Send an H5FP_reply message to process TO.
 * Return:      Success:    SUCCEED
 *              Failure:    FAIL
 * Programmer:  Bill Wendling, 31. July, 2002
 * Modifications:
 */
static herr_t
H5FP_send_reply(unsigned to, unsigned req_id, unsigned file_id, H5FP_status_t status)
{
    H5FP_reply reply;
    herr_t ret_value = SUCCEED;
    int mrc;

    FUNC_ENTER_NOINIT(H5FP_send_reply);

    reply.req_id = req_id;
    reply.file_id = file_id;
    reply.status = status;

    if ((mrc = MPI_Send(&reply, 1, H5FP_reply_t, (int)to, H5FP_TAG_REPLY,
                        H5FP_SAP_COMM)) != MPI_SUCCESS)
        HMPI_GOTO_ERROR(FAIL, "MPI_Send failed", mrc);

done:
    FUNC_LEAVE_NOAPI(ret_value);
}

/*
 * Function:    H5FP_dump
 * Purpose:     Dump all metadata writes to a process so that that
 *              process will write them to the file.
 * Return:      Success:    SUCCEED
 *              Failure:    FAIL
 * Programmer:  Bill Wendling, 31. July, 2002
 * Modifications:
 */
static herr_t
H5FP_dump(H5FP_file_info *info, unsigned to, unsigned req_id, unsigned file_id)
{
    H5FP_read r;
    H5TB_NODE *node;
    herr_t ret_value = SUCCEED;
    int mrc;

    FUNC_ENTER_NOINIT(H5FP_dump);

    assert(info);

    if (!info->mod_tree)
        /* Nothing to write to the file */
        HGOTO_DONE(SUCCEED);

    r.req_id = req_id;
    r.file_id = file_id;
    r.status = H5FP_STATUS_DUMPING;

    node = H5TB_first(info->mod_tree->root);

    while (node) {
        H5FP_mdata_mod *m = (H5FP_mdata_mod *)node->data;

        r.mem_type = m->mem_type;
        r.type_id = m->type_id;
        r.addr = m->addr;
        r.md_size = m->md_size;

        if ((mrc = MPI_Send(&r, 1, H5FP_read_t, (int)to, H5FP_TAG_REPLY,
                            H5FP_SAP_COMM)) != MPI_SUCCESS)
            HMPI_GOTO_ERROR(FAIL, "MPI_Send failed", mrc);

        if (H5FP_send_metadata(m->metadata, (int)m->md_size, (int)to) != SUCCEED)
            HGOTO_ERROR(H5E_FPHDF5, H5E_CANTSENDMDATA, FAIL,
                        "can't dump metadata to client");

        node = H5TB_next(node);
    }

    /* Tell the receiving process that we're finished... */
    r.mem_type = 0;
    r.type_id = 0;
    r.addr = 0;
    r.md_size = 0;
    r.status = H5FP_STATUS_DUMPING_FINISHED;

    if ((mrc = MPI_Send(&r, 1, H5FP_read_t, (int)to, H5FP_TAG_REPLY,
                        H5FP_SAP_COMM)) != MPI_SUCCESS)
        HMPI_GOTO_ERROR(FAIL, "MPI_Send failed", mrc);

    /* Free up the modification tree */
    H5TB_dfree(info->mod_tree, (void (*)(void*))H5FP_free_mod_node, NULL);
    info->mod_tree = NULL;
    info->num_mods = 0;

done:
    FUNC_LEAVE_NOAPI(ret_value);
}

/*
 *===----------------------------------------------------------------------===
 *                    Functions to handle SAP requests
 *===----------------------------------------------------------------------===
 */

/*
 * Function:    H5FP_gen_request_id
 * Purpose:     Generate a unique request ID to send along with a
 *              message.
 * Return:      Integer >= 0 - Doesn't fail.
 * Programmer:  Bill Wendling, 30. July, 2002
 * Modifications:
 */
static unsigned
H5FP_gen_sap_file_id()
{
    static unsigned i = 0;
    FUNC_ENTER_NOINIT(H5FP_gen_sap_file_id);
    FUNC_LEAVE_NOAPI(i++);
}

/*
 * Function:    H5FP_sap_handle_open_request
 * Purpose:     Handle a request to open a file.
 * Return:      Success:    SUCCEED
 *              Failure:    FAIL
 * Programmer:  Bill Wendling, 06. August, 2002
 * Modifications:
 */
static herr_t
H5FP_sap_handle_open_request(H5FP_request req, char *mdata, unsigned UNUSED md_size)
{
    herr_t ret_value = SUCCEED;
    int mrc;

    FUNC_ENTER_NOINIT(H5FP_sap_handle_open_request);

    if (req.obj_type == H5FP_OBJ_FILE) {
        unsigned new_file_id = H5FP_gen_sap_file_id();
        int i;

        if (H5FP_add_new_file_info_to_list(new_file_id, mdata) != SUCCEED)
            HGOTO_ERROR(H5E_FPHDF5, H5E_CANTINSERT, FAIL,
                        "can't insert new file structure to list");

        /* broadcast the file id to all processes */
        /*
         * FIXME: Isn't there some way to broadcast this result to the
         * barrier group? -QAK
         */
        /*
         * XXX:
         *      MPI_Bcast doesn't work in this way and I don't know how
         *      to get it to work for us. From what I gather, all of the
         *      processes need to execute the same bit of code (the
         *      MPI_Bcast function) to get the value to be passed to
         *      everyone. -BW
         */
        for (i = 0; i < H5FP_comm_size; ++i)
            if ((unsigned)i != H5FP_sap_rank)
                if ((mrc = MPI_Send(&new_file_id, 1, MPI_UNSIGNED, i,
                                    H5FP_TAG_FILE_ID, H5FP_SAP_COMM)) != MPI_SUCCESS)
                    /*
                     * FIXME: This is terrible...if we can't send to all
                     * processes, we should clean the file structure from
                     * the list and tell all of the other processes that
                     * we couldn't continue...but how to do that?!?
                     */
                    HMPI_GOTO_ERROR(FAIL, "MPI_Send failed", mrc);
    }

done:
    FUNC_LEAVE_NOAPI(ret_value);
}

/*
 * Function:    H5FP_sap_handle_lock_request
 * Purpose:     Handle a request to lock an object. There are two
 *              different kinds of locks. There are READ and WRITE locks.
 *              The READ locks are sharable, that is if all processes want
 *              to READ the object, they can. They just tell the SAP that
 *              they're doing so and the SAP gives them a "token" to do
 *              that. WRITE locks, on the other hand, are exclusive. You
 *              can't have any outstanding READ or WRITE locks on an
 *              object before you get a WRITE lock on it.
 * Return:      Success:    SUCCEED
 *              Failure:    FAIL
 * Programmer:  Bill Wendling, 06. August, 2002
 * Modifications:
 */
static herr_t
H5FP_sap_handle_lock_request(H5FP_request req)
{
    struct lock_group {
        unsigned char       oid[sizeof(req.oid)];
        unsigned            file_id;
        unsigned            locked;
        H5FP_lock_t         rw_lock;
        H5FP_file_info     *info;
        H5FP_object_lock   *lock;
    } *oids;
    unsigned list_size = 2; /* the size of the "oids" list */
    H5FP_status_t exit_state = H5FP_STATUS_LOCK_ACQUIRED;
    herr_t ret_value = SUCCEED;
    unsigned i, j;

    FUNC_ENTER_NOINIT(H5FP_sap_handle_lock_request);

    if ((oids = (struct lock_group *)HDmalloc(list_size *
                                              sizeof(struct lock_group))) == NULL) {
        exit_state = H5FP_STATUS_OOM;
        HGOTO_ERROR(H5E_RESOURCE, H5E_NOSPACE, FAIL, "out of memory");
    }

    /*
     * Gather together all of the OIDs the process is requesting to lock
     * at one time.
     */
    for (i = 0;; ++i) {
        if (req.oid[0]) {
            if (i == list_size) {
                list_size <<= 1;    /* equiv to list_size *= 2; */
                oids = HDrealloc(oids, list_size * sizeof(struct lock_group));

                if (!oids) {
                    exit_state = H5FP_STATUS_OOM;
                    HGOTO_ERROR(H5E_RESOURCE, H5E_NOSPACE, FAIL, "out of memory");
                }
            }

            H5FP_COPY_OID(oids[i].oid, req.oid);
            oids[i].file_id = req.file_id;
            oids[i].rw_lock = req.rw_lock;
            oids[i].locked = FALSE;
        }

        if (req.req_type == H5FP_REQ_LOCK_END)
            /* this was the last lock request */
            break;

        if (H5FP_sap_receive(&req, (int)req.proc_rank,
                             H5FP_TAG_REQUEST, NULL) != SUCCEED) {
            exit_state = H5FP_STATUS_LOCK_FAILED;
            HGOTO_ERROR(H5E_FPHDF5, H5E_CANTRECV, FAIL, "cannot receive messages");
        }
    }

    /*
     * Check to see if we can acquire all of the locks requested. If
     * not, the it's an error and we need to return.
     */
    for (j = 0; j <= i; ++j) {
        if ((oids[j].info = H5FP_find_file_info(oids[j].file_id)) == NULL) {
            exit_state = H5FP_STATUS_BAD_FILE_ID;
            HGOTO_DONE(FAIL);
        }
        
        if (oids[j].info->closing) {
            /* we're closing the file - don't accept anymore locks */
            exit_state = H5FP_STATUS_FILE_CLOSING;
            HGOTO_DONE(FAIL);
        }

        oids[j].lock = H5FP_find_object_lock(oids[j].info, oids[j].oid);

        /*
         * Don't panic!
         *
         * This horrid little if-then statement is just the logical
         * inverse of the if-then statement in the next for-loop.
         */
        if (oids[j].lock &&
               !((oids[j].rw_lock == H5FP_LOCK_READ &&
                        oids[j].lock->rw_lock == H5FP_LOCK_READ) ||
                 (oids[j].rw_lock == H5FP_LOCK_WRITE &&
                        oids[j].lock->rw_lock == H5FP_LOCK_WRITE &&
                        oids[j].lock->owned_rank == req.proc_rank))) {
            /* FAILURE */
            exit_state = H5FP_STATUS_LOCK_FAILED;
            HGOTO_DONE(FAIL);
        }
    }

    /*
     * Actually acquire the locks. This shouldn't fail because of the
     * previous checks. The only thing which can likely occur is an
     * out-of-memory error.
     */
    for (j = 0; j <= i; ++j) {
        if (oids[j].lock) {
            /*
             * Don't panic!
             *
             * This horrid little if-then statement is just checking so
             * that if you want a READ lock and the current lock is a
             * READ lock, then we bump up the reference count on it. If
             * you want a WRITE lock and the current lock is a WRITE lock
             * and furthermore the current process has that lock, we will
             * also bump up the reference count.
             *
             * Otherwise, it's a failure.
             */
            if ((oids[j].rw_lock == H5FP_LOCK_READ &&
                    oids[j].lock->rw_lock == H5FP_LOCK_READ) ||
                (oids[j].rw_lock == H5FP_LOCK_WRITE &&
                    oids[j].lock->rw_lock == H5FP_LOCK_WRITE &&
                    oids[j].lock->owned_rank == req.proc_rank)) {
                /*
                 * The requesting process may already have this lock. Might
                 * be a request from some call-back function of some sort.
                 * Increase the reference count (which is decreased when
                 * released) to help to prevent deadlocks.
                 */
                ++oids[j].lock->ref_count;
                oids[j].locked = TRUE;
            } else {
                /* FIXME: reply saying object locked? */
                exit_state = H5FP_STATUS_LOCK_FAILED;
                ret_value = FAIL;
                goto rollback;
            }
        } else {
            H5FP_object_lock *lock = H5FP_new_object_lock(oids[j].oid, req.proc_rank,
                                                          req.obj_type, req.rw_lock);

            if (lock) {
                if (!H5TB_dins(oids[j].info->locks, (void *)lock, NULL)) {
                    H5FP_free_object_lock(lock);
                    exit_state = H5FP_STATUS_LOCK_FAILED;
                    ret_value = FAIL;
                    HCOMMON_ERROR(H5E_FPHDF5, H5E_CANTINSERT, "can't insert lock into tree");
                    goto rollback;
                }

                oids[j].locked = TRUE;
            } else {
                /* out of memory...ulp! */
                exit_state = H5FP_STATUS_OOM;
                ret_value = FAIL;
                HCOMMON_ERROR(H5E_RESOURCE, H5E_NOSPACE, "out of memory");
                goto rollback;
            }
        }
    }

    goto done;

    /* Error handling code */
rollback:
    /*
     * More than likely, out of memory during the actual locking phase.
     * Try to release any locks which may have been obtained. If it's not
     * possible to release those locks, we're in big trouble. The file is
     * now in an inconsistent state, as far as the SAP is concerned. The
     * only options left to the program are either to abort or completely
     * close the file and reopen which could cause corruption.
     */
    for (j = 0; j <= i; ++j) {
        if (oids[j].locked) {
            if (oids[j].lock) {
                if (oids[j].lock->owned_rank == req.proc_rank) {
                    if (--oids[j].lock->ref_count == 0) {
                        H5FP_remove_object_lock_from_list(oids[j].info, oids[j].lock);
                    }
                } else {
                    /* CATASTROPHIC FAILURE!!! */
                    /* LOCK WAS NOT CLEARED */
                    exit_state = H5FP_STATUS_CATASTROPHIC;
                }
            } else {
                /* CATASTROPHIC FAILURE!!! */
                /* LOCK WAS NOT CLEARED */
                exit_state = H5FP_STATUS_CATASTROPHIC;
            }
        }
    }

done:
    if (ret_value != SUCCEED) {
        /* Can't lock the whole group at one time for some reason */
HDfprintf(stderr, "%s: locking failure (%d)!!\n", FUNC, ret_value);
    }

    HDfree(oids);
    H5FP_send_reply(req.proc_rank, req.req_id, req.file_id, exit_state);
    FUNC_LEAVE_NOAPI(ret_value);
} 

/*
 * Function:    H5FP_sap_handle_release_lock_request
 * Purpose:     Handle a request to release the lock on an object.
 * Return:      Nothing
 * Programmer:  Bill Wendling, 06. August, 2002
 * Modifications:
 */
static herr_t
H5FP_sap_handle_release_lock_request(H5FP_request req)
{
    struct release_group {
        unsigned char       oid[sizeof(req.oid)];
        unsigned            file_id;
        H5FP_file_info     *info;
        H5FP_object_lock   *lock;
    } *oids;
    unsigned list_size = 2; /* the size of the "oids" list */
    H5FP_status_t exit_state = H5FP_STATUS_LOCK_RELEASED;
    herr_t ret_value;
    unsigned i, j;

    FUNC_ENTER_NOINIT(H5FP_sap_handle_release_lock_request);

    if ((oids = (struct release_group *)HDmalloc(list_size *
                                                 sizeof(struct release_group))) == NULL) {
        exit_state = H5FP_STATUS_OOM;
        HGOTO_ERROR(H5E_RESOURCE, H5E_NOSPACE, FAIL, "out of memory");
    }

    /*
     * Gather together all of the OIDs the process is requesting to
     * release locks at one time.
     */
    for (i = 0;; ++i) {
        if (req.oid[0]) {
            if (i == list_size) {
                list_size <<= 1;    /* equiv to list_size *= 2; */
                oids = HDrealloc(oids, list_size * sizeof(struct release_group));

                if (!oids) {
                    exit_state = H5FP_STATUS_OOM;
                    HGOTO_ERROR(H5E_RESOURCE, H5E_NOSPACE, FAIL, "out of memory");
                }
            }

            H5FP_COPY_OID(oids[i].oid, req.oid);
            oids[i].file_id = req.file_id;
        }

        if (req.req_type == H5FP_REQ_RELEASE_END)
            /* this was the last lock request */
            break;

        if (H5FP_sap_receive(&req, (int)req.proc_rank, H5FP_TAG_REQUEST, NULL) != SUCCEED) {
            exit_state = H5FP_STATUS_LOCK_RELEASE_FAILED;
            HGOTO_DONE(FAIL);
        }
    }

    /*
     * Check here to see if the locks exist and we have the locks. This
     * will help keep us from being in a catastrophic state.
     */
    for (j = 0; j <= i; ++j) {
        if ((oids[j].info = H5FP_find_file_info(oids[j].file_id)) == NULL) {
            exit_state = H5FP_STATUS_BAD_FILE_ID;
            HGOTO_DONE(FAIL);
        }

        oids[j].lock = H5FP_find_object_lock(oids[j].info, oids[j].oid);

        if (!oids[j].lock || oids[j].lock->owned_rank != req.proc_rank) {
            exit_state = H5FP_STATUS_BAD_LOCK;
            HGOTO_DONE(FAIL);
        }
    }

    /*
     * Release a lock. There may be multiple locks to release if they
     * were locked in a group, so loop finding all of the locks and
     * release them.
     */
    for (j = 0; j <= i; ++j) {
        if (oids[j].lock) {
            if (oids[j].lock->owned_rank == req.proc_rank) {
                if (--oids[j].lock->ref_count == 0)
                    H5FP_remove_object_lock_from_list(oids[j].info, oids[j].lock);
            } else {
                /* AAAIIIIEEE!!! */
                exit_state = H5FP_STATUS_CATASTROPHIC;
                HGOTO_DONE(FAIL);
            }
        } else {
            /* AAAIIIIEEE!!! */
            exit_state = H5FP_STATUS_CATASTROPHIC;
            HGOTO_DONE(FAIL);
        }
    }

done:
    HDfree(oids);
    H5FP_send_reply(req.proc_rank, req.req_id, req.file_id, exit_state);
    FUNC_LEAVE_NOAPI(ret_value);
}

/*
 * Function:    H5FP_sap_handle_read_request
 * Purpose:     Handle a read request from a client. The bit of metadata
 *              that the client wants may or may not be in here. It's not
 *              an error if it isn't here. When that's the case, the
 *              client just goes and reads it from the file.
 * Return:      Success:    SUCCEED
 *              Failure:    FAIL
 * Programmer:  Bill Wendling, 06. August, 2002
 * Modifications:
 */
static herr_t
H5FP_sap_handle_read_request(H5FP_request req)
{
    H5FP_file_info *info;
    H5FP_read r;
    herr_t ret_value = SUCCEED;
    char *metadata = NULL;
    int mrc;

    FUNC_ENTER_NOINIT(H5FP_sap_handle_read_request);

    r.req_id = req.req_id;
    r.file_id = req.file_id;
    r.md_size = 0;
    r.type_id = 0;
    r.addr = 0;
    r.status = H5FP_STATUS_MDATA_NOT_CACHED;

    if ((info = H5FP_find_file_info(req.file_id)) != NULL) {
        H5FP_mdata_mod mod;     /* Used to find the correct modification */
        H5TB_NODE *node;

        if (info->num_mods >= H5FP_MDATA_CACHE_HIGHWATER_MARK) {
            if (H5FP_dump(info, req.proc_rank, req.req_id, req.file_id) == FAIL)
                HGOTO_ERROR(H5E_FPHDF5, H5E_CANTSENDMDATA, FAIL,
                            "can't dump metadata to client");

            /*
             * We aren't going to find the information we need since it
             * was just dumped.
             */
            HGOTO_DONE(SUCCEED);
        }

        mod.addr = req.addr;    /* This is the key field for the TBBT */

        if ((node = H5TB_dfind(info->mod_tree, (void *)&mod, NULL)) != NULL) {
            H5FP_mdata_mod *fm = (H5FP_mdata_mod *)node->data;

            r.md_size = fm->md_size;
            r.type_id = fm->type_id;
            r.addr = fm->addr;
            r.status = H5FP_STATUS_OK;
            metadata = fm->metadata;    /* Sent out in a separate message */
        }
    }

    if ((mrc = MPI_Send(&r, 1, H5FP_read_t, (int)req.proc_rank,
                        H5FP_TAG_READ, H5FP_SAP_COMM)) != MPI_SUCCESS)
        HMPI_GOTO_ERROR(FAIL, "MPI_Send failed", mrc);

    if (r.md_size)
        if (H5FP_send_metadata(metadata, (int)r.md_size, (int)req.proc_rank) != SUCCEED)
            HGOTO_ERROR(H5E_FPHDF5, H5E_CANTSENDMDATA, FAIL,
                        "can't send metadata to client");

done:
    FUNC_LEAVE_NOAPI(ret_value);
}

/*
 * Function:    H5FP_sap_handle_write_request
 * Purpose:     Handle a request to write a piece of metadata in the
 *              file.
 * Return:      Success:    SUCCEED
 *              Failure:    FAIL
 * Programmer:  Bill Wendling, 06. August, 2002
 * Modifications:
 */
static herr_t
H5FP_sap_handle_write_request(H5FP_request req, char *mdata, unsigned md_size)
{
    H5FP_file_info *info;
    H5FP_status_t exit_state = H5FP_STATUS_OK;
    herr_t ret_value = SUCCEED;

    FUNC_ENTER_NOINIT(H5FP_sap_handle_write_request);

    if ((info = H5FP_find_file_info(req.file_id)) != NULL) {
        if (info->num_mods >= H5FP_MDATA_CACHE_HIGHWATER_MARK) {
            /*
             * If there are any modifications not written out yet, dump
             * them to this process
             */
            if (H5FP_send_reply(req.proc_rank, req.req_id, req.file_id,
                                H5FP_STATUS_DUMPING) == FAIL) {
                exit_state = H5FP_STATUS_DUMPING_FAILED;
                HGOTO_ERROR(H5E_FPHDF5, H5E_CANTSENDMDATA, FAIL,
                            "can't send message to client");
            }

            if (H5FP_dump(info, req.proc_rank, req.req_id, req.file_id) == FAIL) {
                exit_state = H5FP_STATUS_DUMPING_FAILED;
                HGOTO_ERROR(H5E_FPHDF5, H5E_CANTSENDMDATA, FAIL,
                            "metadata dump failed");
            }
        }

        if (info->closing) {
            /* we're closing the file - don't accept anymore changes */
            exit_state = H5FP_STATUS_FILE_CLOSING;
            ret_value = FAIL;
        } else {
            /* handle the change request */
            H5FP_object_lock *lock = H5FP_find_object_lock(info, req.oid);

            if (!lock || lock->owned_rank != req.proc_rank
                    || lock->rw_lock != H5FP_LOCK_WRITE) {
                /*
                 * There isn't a write lock or we don't own the write
                 * lock on this OID
                 */
                exit_state = H5FP_STATUS_NO_LOCK;
                ret_value = FAIL;
            } else if (H5FP_add_file_mod_to_list(info, req.mem_type, req.type_id,
                                                 req.addr, req.proc_rank, md_size,
                                                 mdata) != SUCCEED) {
                exit_state = H5FP_STATUS_OOM;
                ret_value = FAIL;
            }
        }
    } else {
        /* error: there isn't a file opened to change */
        exit_state = H5FP_STATUS_BAD_FILE_ID;
        ret_value = FAIL;
    }

done:
    H5FP_send_reply(req.proc_rank, req.req_id, req.file_id, exit_state);
    FUNC_LEAVE_NOAPI(ret_value);
}

/*
 * Function:    H5FP_sap_handle_close_request
 * Purpose:     Handle a request to close a file.
 * Return:      Success:    SUCCEED
 *              Failure:    FAIL
 * Programmer:  Bill Wendling, 06. August, 2002
 * Modifications:
 */
static herr_t
H5FP_sap_handle_close_request(H5FP_request req)
{
    H5FP_file_info *info;
    H5FP_status_t exit_state = H5FP_STATUS_OK;
    herr_t ret_value = SUCCEED;

    FUNC_ENTER_NOINIT(H5FP_sap_handle_close_request);

    if ((info = H5FP_find_file_info(req.file_id)) != NULL) {
        if (info->num_mods) {
            /*
             * If there are any modifications not written out yet, dump
             * them to this process
             */
            if (H5FP_send_reply(req.proc_rank, req.req_id, req.file_id,
                                H5FP_STATUS_DUMPING) == FAIL) {
                exit_state = H5FP_STATUS_DUMPING_FAILED;
                HGOTO_ERROR(H5E_FPHDF5, H5E_CANTSENDMDATA, FAIL,
                            "can't send message to client");
            }

            if (H5FP_dump(info, req.proc_rank, req.req_id, req.file_id) == FAIL) {
                exit_state = H5FP_STATUS_DUMPING_FAILED;
                HGOTO_ERROR(H5E_FPHDF5, H5E_CANTSENDMDATA, FAIL,
                            "can't dump metadata to client");
            }
        }

        if (++info->closing == H5FP_comm_size - 1)
            /* all processes have closed the file - remove it from list */
            if (H5FP_remove_file_id_from_list(req.file_id) != SUCCEED) {
                exit_state = H5FP_STATUS_BAD_FILE_ID;
                HGOTO_ERROR(H5E_FPHDF5, H5E_NOTFOUND, FAIL,
                            "cannot remove file ID from list");
            }
    }

done:
    H5FP_send_reply(req.proc_rank, req.req_id, req.file_id, exit_state);
    FUNC_LEAVE_NOAPI(ret_value);
}

#endif  /* H5_HAVE_FPHDF5 */
