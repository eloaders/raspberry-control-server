/* Raspberry Control - Control Raspberry Pi with your Android Device
 *
 * Copyright (C) Lukasz Skalski <lukasz.skalski@op.pl>
 * Copyright (C) Maciej Wereski <maciekwer@wp.pl>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#ifndef __RCS_UTIL
#define __RCS_UTIL

/* Macros for arrays */

#define FREE_ARRAY_ELEMENTS(arr, i, n) \
	for(i = 0; i < n; ++i) { \
		free(arr[i]); \
	}

#endif /* __RCS_UTIL */
