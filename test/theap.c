/*-------------------------------------------------------------------------
 * Copyright (C) 1997	National Center for Supercomputing Applications.
 *                      All rights reserved.
 *
 *-------------------------------------------------------------------------
 *
 * Created:		theap.c
 * 			Jul 17 1997
 * 			Robb Matzke <robb@maya.nuance.com>
 *
 * Purpose:		
 *
 * Modifications:	
 *
 *-------------------------------------------------------------------------
 */
#include "testhdf5.h"

#include "H5ACprivate.h"
#include "H5Fprivate.h"
#include "H5Hprivate.h"

#define NOBJS	40


/*-------------------------------------------------------------------------
 * Function:	test_heap
 *
 * Purpose:	Test name and object heaps.
 *
 * Return:	void
 *
 * Programmer:	Robb Matzke
 *		robb@maya.nuance.com
 *		Jul 17 1997
 *
 * Modifications:
 *
 *-------------------------------------------------------------------------
 */
void
test_heap (void)
{
   int		i, j;
   hatom_t	fid;
   hdf5_file_t	*f;
   off_t	heap;
   char		buf[NOBJS+8];
   const char	*s;
   off_t	obj[NOBJS];
   
   MESSAGE (5, print_func("Testing Heaps\n"););

   /* Create the file */
   fid = H5Fcreate ("theap.h5", H5ACC_OVERWRITE, 0, 0);
   CHECK (fid, FAIL, "H5Fcreate");
   f = H5Aatom_object (fid);
   CHECK (f, NULL, "H5Aatom_object");

   /* Create a new heap */
   heap = H5H_new (f, H5H_LOCAL, 0);
   CHECK_I (heap, "H5H_new");

   /* Add stuff to the heap */
   for (i=0; i<NOBJS; i++) {
      sprintf (buf, "%03d-", i);
      for (j=4; j<i; j++) buf[j] = '0' + j%10;
      if (j>4) buf[j] = '\0';

      obj[i] = H5H_insert (f, heap, strlen(buf)+1, buf);
      CHECK_I (heap, "H5H_insert");
   }

   /* Flush the cache and invalidate everything */
   H5AC_flush (f, NULL, 0, TRUE);

   /* Read the objects back out */
   for (i=0; i<NOBJS; i++) {
      s = H5H_peek (f, heap, obj[i]);
      MESSAGE (8, print_func ("object is `%s'\n", s););
   }

   /* Close the file */
   H5Fclose (fid);
}

