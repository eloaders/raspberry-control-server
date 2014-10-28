#ifndef __RPS_UTIL
#define __RPS_UTIL

/* Macros for arrays */

#define FREE_ARRAY_ELEMENTS(arr, i, n) \
	for(i = 0; i < n; ++i) { \
		free(arr[i]); \
	}

#endif /* __RPS_UTIL */
