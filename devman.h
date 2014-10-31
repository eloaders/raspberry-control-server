#ifndef __DEVMAN_H
#define __DEVMAN_H
#include <stdbool.h>
#include <time.h>

struct utsname;
struct sysinfo;

struct devman_ctx {
	struct utsname *uname;
	struct sysinfo *sysinfo;
	time_t last_update;
};

struct devman_ctx *devman_ctx_init(void);
void devman_ctx_free(struct devman_ctx *ctx);
int devman_ctx_update(struct devman_ctx *ctx);

char *get_kernel_version(const struct devman_ctx *ctx);
char *get_uptime_str(const struct devman_ctx *ctx);
char *get_cpuload_str(const struct devman_ctx *ctx);
char *get_rpi_serial(void);
int get_netdevices(char ***devices, bool (*filter)(const char *));
int get_df(char ***filesystems, bool (*filter)(const char *));
double total_mem_usage(const struct devman_ctx *ctx, bool swap);
double total_cpu_usage(void);

#endif /* __DEVMAN_H */
