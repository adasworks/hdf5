/*
 * Copyright (C) 2000-2002 NCSA
 *		           All rights reserved.
 *
 * Programmer:	Quincey Koziol <koziol@ncsa.uiuc.edu>
 *		Thursday, September 28, 2000
 *
 * Purpose:	This file contains declarations which are visible only within
 *		the H5F package.  Source files outside the H5F package should
 *		include H5Fprivate.h instead.
 */
#ifndef H5F_PACKAGE
#error "Do not include this file outside the H5F package!"
#endif

#ifndef _H5Fpkg_H
#define _H5Fpkg_H

#include "H5Fprivate.h"
#include "H5FOprivate.h"        /* File objects                         */

/* This is a near top-level header! Try not to include much! */
#include "H5private.h"

#include "H5Bpublic.h"          /* B-tree header, for H5B_NUM_BTREE_ID */

/*
 * Feature: Define this constant to be non-zero if you want to enable code
 *	    that minimizes the number of calls to lseek().  This has a huge
 *	    performance benefit on some systems.  Set this constant to zero
 *	    on the compiler command line to disable that optimization.
 */
#ifndef H5F_OPT_SEEK
#  define H5F_OPT_SEEK 1
#endif

/*
 * Feature: Define this constant on the compiler command-line if you want to
 *	    see some debugging messages on the debug stream.
 */
#ifdef NDEBUG
#  undef H5F_DEBUG
#endif

/* Maximum size of boot-block buffer */
#define H5F_BOOTBLOCK_SIZE  1024

/* Define the HDF5 file signature */
#define H5F_SIGNATURE	  "\211HDF\r\n\032\n"
#define H5F_SIGNATURE_LEN 8

/*
 * Private file open flags.
 */
#define H5F_ACC_PUBLIC_FLAGS 	0x00ffu

/*
 * Macros that check for overflows.  These are somewhat dangerous to fiddle
 * with.
 */
#if (H5_SIZEOF_SIZE_T >= H5_SIZEOF_OFF_T)
#   define H5F_OVERFLOW_SIZET2OFFT(X)					      \
    ((size_t)(X)>=(size_t)((size_t)1<<(8*sizeof(off_t)-1)))
#else
#   define H5F_OVERFLOW_SIZET2OFFT(X) 0
#endif
#if (H5_SIZEOF_HSIZE_T >= H5_SIZEOF_OFF_T)
#   define H5F_OVERFLOW_HSIZET2OFFT(X)					      \
    ((hsize_t)(X)>=(hsize_t)((hsize_t)1<<(8*sizeof(off_t)-1)))
#else
#   define H5F_OVERFLOW_HSIZET2OFFT(X) 0
#endif

/* The raw data chunk cache */
typedef struct H5F_rdcc_t {
    unsigned		ninits;	/* Number of chunk creations		*/
    unsigned		nhits;	/* Number of cache hits			*/
    unsigned		nmisses;/* Number of cache misses		*/
    unsigned		nflushes;/* Number of cache flushes		*/
    size_t		nbytes;	/* Current cached raw data in bytes	*/
    size_t		nslots;	/* Number of chunk slots allocated	*/
    struct H5F_rdcc_ent_t *head; /* Head of doubly linked list		*/
    struct H5F_rdcc_ent_t *tail; /* Tail of doubly linked list		*/
    int		nused;	/* Number of chunk slots in use		*/
    struct H5F_rdcc_ent_t **slot; /* Chunk slots, each points to a chunk*/
} H5F_rdcc_t;

/*
 * Define the structure to store the file information for HDF5 files. One of
 * these structures is allocated per file, not per H5Fopen(). That is, set of
 * H5F_t structs can all point to the same H5F_file_t struct. The `nrefs'
 * count in this struct indicates the number of H5F_t structs which are
 * pointing to this struct.
 */
typedef struct H5F_file_t {
    unsigned	flags;		/* Access Permissions for file		*/
    H5FD_t	*lf; 		/* Lower level file handle for I/O	*/
    unsigned	nrefs;		/* Ref count for times file is opened	*/
    uint32_t	consist_flags;	/* File Consistency Flags		*/

    /* Cached values from FCPL */
    size_t	sizeof_addr;	/* Size of addresses in file            */
    size_t	sizeof_size;	/* Size of offsets in file              */
    unsigned	sym_leaf_k;	/* Size of leaves in symbol tables      */
    int btree_k[H5B_NUM_BTREE_ID];  /* B-tree key values for each type  */

    haddr_t	boot_addr;	/* Absolute address of boot block	*/
    haddr_t	base_addr;	/* Absolute base address for rel.addrs. */
    haddr_t	freespace_addr;	/* Relative address of free-space info	*/
    haddr_t	driver_addr;	/* File driver information block address*/
    unsigned	boot_chksum;	/* Boot block checksum                  */
    unsigned	drvr_chksum;	/* Driver info block checksum           */
    struct H5AC_t *cache;	/* The object cache			*/
    hid_t       fcpl_id;	/* File creation property list ID 	*/
    int	mdc_nelmts;		/* Size of meta data cache (elements)	*/
    size_t	rdcc_nelmts;	/* Size of raw data chunk cache (elmts)	*/
    size_t	rdcc_nbytes;	/* Size of raw data chunk cache	(bytes)	*/
    double	rdcc_w0;	/* Preempt read chunks first? [0.0..1.0]*/
    hsize_t	threshold;	/* Threshold for alignment		*/
    hsize_t	alignment;	/* Alignment				*/
    unsigned	gc_ref;		/* Garbage-collect references?		*/
    struct H5G_t *root_grp;	/* Open root group			*/
    int	ncwfs;			/* Num entries on cwfs list		*/
    struct H5HG_heap_t **cwfs;	/* Global heap cache			*/
    H5FO_t *open_objs;          /* Open objects in file                 */

    /* Data Sieve Buffering fields */
    unsigned char *sieve_buf;   /* Buffer to hold data sieve buffer */
    haddr_t sieve_loc;          /* File location (offset) of the data sieve buffer */
    size_t sieve_size;          /* Size of the data sieve buffer used (in bytes) */
    size_t sieve_buf_size;      /* Size of the data sieve buffer allocated (in bytes) */
    unsigned sieve_dirty;       /* Flag to indicate that the data sieve buffer is dirty */

    H5F_rdcc_t	rdcc;		/* Raw data chunk cache			*/
    H5F_close_degree_t fc_degree;   /* File close behavior degree	*/
} H5F_file_t;

/* A record of the mount table */
typedef struct H5F_mount_t {
    struct H5G_t	*group;	/* Mount point group held open		*/
    struct H5F_t	*file;	/* File mounted at that point		*/
} H5F_mount_t;
    
/*
 * The mount table describes what files are attached to (mounted on) the file
 * to which this table belongs.
 */
typedef struct H5F_mtab_t {
    struct H5F_t	*parent;/* Parent file				*/
    unsigned		nmounts;/* Number of children which are mounted	*/
    unsigned		nalloc;	/* Number of mount slots allocated	*/
    H5F_mount_t		*child;	/* An array of mount records		*/
} H5F_mtab_t;

/*
 * This is the top-level file descriptor.  One of these structures is
 * allocated every time H5Fopen() is called although they may contain pointers
 * to shared H5F_file_t structs. The reference count (nrefs) indicates the
 * number of times the file has been opened (the application can only open a
 * file once explicitly, but the library can open the file a second time to
 * indicate that the file is mounted on some other file).
 */
struct H5F_t {
    unsigned		nrefs;		/* Reference count		*/
    unsigned		intent;		/* The flags passed to H5F_open()*/
    char		*name;		/* Name used to open file	*/
    H5F_file_t		*shared;	/* The shared file info		*/
    unsigned		nopen_objs;	/* Number of open object headers*/
    hid_t		closing;	/* H5I_FILE_CLOSING ID or zero	*/
    H5F_mtab_t		mtab;		/* File mount table		*/
};

#ifdef H5_HAVE_PARALLEL
H5_DLLVAR  hbool_t H5_mpi_1_metawrite_g;
H5_DLLVAR  hbool_t H5_mpiposix_1_metawrite_g;
#endif /* H5_HAVE_PARALLEL */

/* Private functions, not part of the publicly documented API */
#ifdef NOT_YET
H5_DLL void H5F_encode_length_unusual(const H5F_t *f, uint8_t **p, uint8_t *l);
#endif /* NOT_YET */
H5_DLL herr_t H5F_mountpoint(struct H5G_entry_t *find/*in,out*/);
H5_DLL herr_t H5F_debug(H5F_t *f, hid_t dxpl_id, haddr_t addr, FILE * stream,
			 int indent, int fwidth);

/* Functions that operate on indexed storage */
H5_DLL herr_t H5F_istore_init (H5F_t *f);
H5_DLL herr_t H5F_istore_flush (H5F_t *f, hid_t dxpl_id, hbool_t preempt);
H5_DLL herr_t H5F_istore_dest (H5F_t *f, hid_t dxpl_id);
H5_DLL herr_t H5F_istore_read(H5F_t *f, hid_t dxpl_id,
                                const struct H5O_layout_t *layout,
                                struct H5P_genplist_t *dc_plist,
                                const hsize_t size_m[], const hssize_t offset_m[],
                                const hssize_t offset_f[], const hsize_t size[],
                                void *buf/*out*/);
H5_DLL herr_t H5F_istore_write(H5F_t *f, hid_t dxpl_id,
                                const struct H5O_layout_t *layout,
                                struct H5P_genplist_t *dc_plist,
                                const hsize_t size_m[], const hssize_t offset_m[],
                                const hssize_t offset[], const hsize_t size[],
                                const void *buf);
H5_DLL herr_t H5F_istore_stats (H5F_t *f, hbool_t headers);
H5_DLL herr_t H5F_istore_debug(H5F_t *f, hid_t dxpl_id, haddr_t addr, FILE * stream,
				int indent, int fwidth, int ndims);

/* Functions that operate on contiguous storage wrt boot block */
H5_DLL herr_t H5F_contig_read(H5F_t *f, hsize_t max_data, H5FD_mem_t type, haddr_t addr,
        size_t size, hid_t dxpl_id, void *_buf/*out*/);
H5_DLL herr_t H5F_contig_write(H5F_t *f, hsize_t max_data, H5FD_mem_t type, haddr_t addr,
        size_t size, hid_t dxpl_id, const void *buf);
H5_DLL herr_t H5F_contig_readv(H5F_t *f, hsize_t max_data, H5FD_mem_t type, haddr_t addr,
        size_t nseq, size_t size[], hsize_t offset[], hid_t dxpl_id, void *_buf/*out*/);
H5_DLL herr_t H5F_contig_writev(H5F_t *f, hsize_t max_data, H5FD_mem_t type, haddr_t addr,
        size_t nseq, size_t size[], hsize_t offset[], hid_t dxpl_id, const void *buf);

/* Functions that operate on compact dataset storage */
H5_DLL herr_t H5F_compact_readv(H5F_t *f, const struct H5O_layout_t *layout, size_t nseq,      
                                size_t size_arr[], hsize_t offset_arr[],
                                hid_t dxpl_id, void *_buf/*out*/);
H5_DLL herr_t H5F_compact_writev(H5F_t *f, struct H5O_layout_t *layout, size_t nseq,
                                size_t size_arr[], hsize_t offset_arr[], 
                                hid_t dxpl_id, const void *_buf);

                                    
#endif

