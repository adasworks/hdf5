
/****************************************************************************
 * NCSA HDF                                                                 *
 * Scientific Data Technologies                                             *
 * National Center for Supercomputing Applications                          *
 * University of Illinois at Urbana-Champaign                               *
 * 605 E. Springfield, Champaign IL 61820                                   *
 *                                                                          *
 * For conditions of distribution and use, see the accompanying             *
 * hdf/COPYING file.                                                        *
 *                                                                          *
 ****************************************************************************/


#ifndef _H5DS_H
#define _H5DS_H

#include <hdf5.h>

#ifndef TRUE
#define TRUE 1
#endif

#define DIMENSION_SCALE_CLASS "DIMENSION_SCALE"
#define DIMENSION_LIST        "DIMENSION_LIST"
#define REFERENCE_LIST        "REFERENCE_LIST"
#define DIMENSION_LABELS      "DIMENSION_LABELS"

typedef herr_t (*H5DS_iterate_t)(hid_t dset, unsigned dim, hid_t scale, void *visitor_data); 


/* attribute type of a DS dataset */
typedef struct ds_list_t {
 hobj_ref_t ref;     /* object reference  */
 int        dim_idx; /* dimension index of the dataset */
} ds_list_t;


#ifdef __cplusplus
extern "C" {
#endif

herr_t H5DSattach_scale(hid_t did,
                        hid_t dsid,
                        unsigned int idx);

herr_t H5DSdetach_scale(hid_t did,
                        hid_t dsid,
                        unsigned int idx);

herr_t H5DSset_scale(hid_t did, 
                     char *dimname);

herr_t H5DSget_nscales(hid_t did,
                       unsigned int dim,
                       int *nscales);

herr_t H5DSset_label(hid_t did, 
                     char *label,
                     unsigned int idx);

herr_t H5DSget_label(hid_t did, 
                     char *label,
                     unsigned int idx);

herr_t H5DSget_scale_name(hid_t did, 
                          char *buf);

herr_t H5DSis_scale(hid_t did);

herr_t H5DSiterate_scales(hid_t did, 
                          unsigned int dim, 
                          int *idx, 
                          H5DS_iterate_t visitor, 
                          void *visitor_data);


/*-------------------------------------------------------------------------
 * private functions
 *-------------------------------------------------------------------------
 */
htri_t H5DS_is_attached(hid_t loc_id, 
                        const char *dname,
                        const char *dsname,
                        unsigned int idx);


#ifdef __cplusplus
}
#endif

#endif
