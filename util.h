#ifndef __RCS_UTIL
#define __RCS_UTIL

/* Macros for arrays */

#define FREE_ARRAY_ELEMENTS(arr, i, n) \
	for(i = 0; i < n; ++i) { \
		free(arr[i]); \
	}

#endif /* __RCS_UTIL */
