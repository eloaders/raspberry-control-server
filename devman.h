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

#ifndef __DEVMAN_H
#define __DEVMAN_H
#include <stdbool.h>

struct devman_ctx;

struct devman_ctx *devman_ctx_init(void);
void devman_ctx_free(struct devman_ctx *ctx);
int devman_ctx_update(struct devman_ctx *ctx);

const char *get_board_serial(const struct devman_ctx *ctx);
const char *get_board_revision(const struct devman_ctx *ctx);
char *get_kernel_version(const struct devman_ctx *ctx);
char *get_uptime_str(const struct devman_ctx *ctx);
char *get_cpuload_str(const struct devman_ctx *ctx);
int get_board_cpu_temp(void);
int get_netdevices(char ***devices, bool (*filter)(const char *));
int get_df(char ***filesystems, bool (*filter)(const char *));
double total_mem_usage(const struct devman_ctx *ctx, bool swap);
double total_cpu_usage(void);

/* GPIO */
//FIXME: gpio_control should be static in .c? change it to macros?
int gpio_control(unsigned int pin, bool enable);
static inline int gpio_enable(unsigned int pin)
{
	return gpio_control(pin, 1);
}
static inline int gpio_disable(unsigned int pin)
{
	return gpio_control(pin, 0);
}
int gpio_set_value(unsigned int pin, unsigned int value);
int gpio_get_value(unsigned int pin);
int gpio_set_direction(unsigned int pin, const char *direction);
char *gpio_get_direction(unsigned int pin);

#endif /* __DEVMAN_H */
