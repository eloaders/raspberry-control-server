/* Welcome in the land of OCD! */
#include "util.h"
#include "devman.h"

#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <dirent.h>
#include <limits.h>
#include <mntent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/statfs.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>

struct devman_ctx *devman_ctx_init(void)
{
	struct devman_ctx *ctx;
	ctx = malloc(sizeof(*ctx));

	if (ctx == NULL)
		return NULL;

	ctx->uname = malloc(sizeof(*ctx->uname));
	ctx->sysinfo = malloc(sizeof(*ctx->sysinfo));
	ctx->last_update = 0;

	if (ctx->uname == NULL || ctx->sysinfo == NULL || devman_ctx_update(ctx) < 0) {
		free(ctx->uname);
		free(ctx->sysinfo);
		free(ctx);
		ctx = NULL;
	}

	return ctx;
}

void devman_ctx_free(struct devman_ctx *ctx)
{
	assert(ctx);
	free(ctx->uname);
	free(ctx->sysinfo);
	free(ctx);
}

int devman_ctx_update(struct devman_ctx *ctx)
{
	assert(ctx);

	time_t t = time(NULL);

	/*TODO: this should be configurable. */
	if (t - ctx->last_update < 120)
		return 0;

	if (uname(ctx->uname) < 0)
		return -1;

	if (sysinfo(ctx->sysinfo) < 0)
		return -1;

	ctx->last_update = t;

	return 0;
}

char *get_kernel_version(const struct devman_ctx *ctx)
{
	assert(ctx);
	return strdup(ctx->uname->release);
}

char *get_uptime_str(const struct devman_ctx *ctx)
{
	unsigned int hrs, min, sec;
	char *uptime;

	assert(ctx);

	uptime = malloc(LINE_MAX);
	if (uptime == NULL)
		return NULL;

	hrs = ctx->sysinfo->uptime / 3600;
	min = ctx->sysinfo->uptime / 60 - hrs * 60;
	sec = ctx->sysinfo->uptime - 60 * (hrs * 60 + min);
	assert(min < 60 && sec < 60);

	snprintf(uptime, LINE_MAX, "%uh %um %us", hrs, min, sec);
	return uptime;
}

char *get_cpuload_str(const struct devman_ctx *ctx)
{
	char *load; 

	assert(ctx);
		
	load = malloc(LINE_MAX);
	if (load == NULL)
		return NULL;

	snprintf(load, LINE_MAX, "%#.2g %#.2g %#.2g",
			(double)(ctx->sysinfo->loads[0] / (double)(1 << SI_LOAD_SHIFT)),
			(double)(ctx->sysinfo->loads[1] / (double)(1 << SI_LOAD_SHIFT)),
			(double)(ctx->sysinfo->loads[2] / (double)(1 << SI_LOAD_SHIFT)));

	return load;
}

char *get_rpi_serial(void)
{
	FILE *fp;
	char *serial;
	char line[LINE_MAX] = {0,};

	fp = fopen("/proc/cpuinfo", "r");
	if (fp == NULL)
		return NULL;

	serial = malloc(LINE_MAX * sizeof(*serial));
	if (serial == NULL)
		goto end;

	while (fgets(line, LINE_MAX, fp))
		if (sscanf(line, "%*[Ss]erial : %s\n", serial))
			break;

	if (ferror(fp)) {
		free(serial);
		serial = NULL;
	}
end:
	fclose(fp);
	return serial;
}

int get_netdevices(char ***devices, bool (*filter)(const char *))
{
	DIR *sys;
	struct dirent *ent;
	FILE *fp = NULL;
	char **arr = NULL;
	char path[PATH_MAX] = {0,};
	char addr[18] = {[17]0};
	int r = 0, n = 0;

	sys = opendir("/sys/class/net");
	if (sys == NULL)
		return -1;

	errno = 0;
	for(ent = readdir(sys); ent; ent = readdir(sys)) {
		if (ent->d_name[0] == '.')
			continue;
		++n;
	}
	if (errno)
		goto fail;

	arr = malloc(n * sizeof(*arr));
	if (arr == NULL)
		goto fail;

	rewinddir(sys);
	errno = 0;
	for (ent = readdir(sys); ent; ent = readdir(sys)) {
		if ((filter && !filter(ent->d_name)) || ent->d_name[0] == '.')
			continue;

		if (snprintf(path, PATH_MAX, "/sys/class/net/%s/address", ent->d_name) < 0)
			goto fail;
		fp = fopen(path, "r");
		if (fp == NULL)
			goto fail;

		if (fscanf(fp, "%17s\n", addr) < 1)
			goto fail;

		arr[r] = malloc(20+strlen(ent->d_name)); /* addr + ":" + " "  */
		if (arr[r] == NULL) {
			++r;
			goto fail;
		}

		sprintf(arr[r++], "%s: %s", ent->d_name, addr);
		fclose(fp);
		fp = NULL;
	}
	if (errno)
		goto fail;

	*devices = realloc(arr, r * sizeof(*arr));
	if (*devices == NULL)
		goto fail;
	closedir(sys);
	return r; /* number of allocated elements  */
fail:
	n = errno;
	closedir(sys);
	if (fp)
		fclose(fp);
	errno = n;
	if (arr)
		FREE_ARRAY_ELEMENTS(arr, n, r);
	free(arr);
	return -1;
}

/*TODO: filter should accept mntent structure instead of char *. */
int get_df(char ***filesystems, bool (*filter)(const char *))
{
	struct statfs sfs;
	struct mntent *ent;
	uint64_t bytes_used, bytes_free;
	int n = 0, r = 0;
	char **arr = NULL;
	FILE *fp = setmntent("/etc/mtab", "r");
	if (fp == NULL)
		return -1;

	for (ent = getmntent(fp); ent; ent = getmntent(fp))
		++n;

	arr = malloc(n * sizeof(*arr));
	if (arr == NULL)
		goto fail;

	rewind(fp);

	for (ent = getmntent(fp); ent; ent = getmntent(fp)) {
		if (filter && !filter(ent->mnt_dir))
			continue;

		if (statfs(ent->mnt_dir, &sfs) < 0 )
			goto fail;

		arr[r] = malloc(LINE_MAX);
		if (arr[r] == NULL)
			goto fail;

		bytes_free = sfs.f_bfree * sfs.f_bsize;
		bytes_used = (sfs.f_blocks - sfs.f_bfree) * sfs.f_bsize;
		sprintf(arr[r++], "%s %s %"PRIu64" %"PRIu64, ent->mnt_fsname, ent->mnt_dir, bytes_used, bytes_free);
	}
	*filesystems = realloc(arr, r * sizeof(*arr));
	if (*filesystems == NULL)
		goto fail;
	endmntent(fp);
	return r;
fail:
	endmntent(fp);
	if (arr)
		FREE_ARRAY_ELEMENTS(arr, n, r);
	free(arr);
	return -1;
};

double total_mem_usage(const struct devman_ctx *ctx, bool swap)
{
	assert(ctx);
	
	if (swap)
		return (double) (ctx->sysinfo->totalswap - ctx->sysinfo->freeswap)
			/ ctx->sysinfo->totalswap * 100.0;

	return (double) (ctx->sysinfo->totalram - ctx->sysinfo->freeram)
			/ ctx->sysinfo->totalram * 100.0;
}

enum {
	CPU_STATE_USER,
	CPU_STATE_NICE,
	CPU_STATE_SYSTEM,
	CPU_STATE_IDLE,
	CPU_STATE_IOWAIT,
	CPU_STATE_IRQ,
	CPU_STATE_SOFTIRQ,
	CPU_STATE_STEAL,
	CPU_STATE_GUEST,
	CPU_STATE_GUEST_NICE,
	_CPU_STATE_COUNT
};

#define cpu_stats_to_arr(fp, arr) \
		fscanf(fp, "cpu %"SCNu64"%"SCNu64"%"SCNu64"%"SCNu64"%"SCNu64"%"SCNu64"%"SCNu64"%"SCNu64"%"SCNu64"%"SCNu64"\n", \
				&arr[0], &arr[1], &arr[2], &arr[3], &arr[4], &arr[5], &arr[6], &arr[7], &arr[8], &arr[9])

double total_cpu_usage(void)
{
	FILE *fp;
	int r;
	uint64_t total1, work1, total2, work2;
	uint64_t vals1[_CPU_STATE_COUNT] = {0,};
	uint64_t vals2[_CPU_STATE_COUNT] = {0,};

	fp = fopen("/proc/stat","r");
	if (fp == NULL)
		return -1.0;

	r = cpu_stats_to_arr(fp, vals1);
	if (r < 4 || (r == EOF && ferror(fp)))
		goto fail;

	rewind(fp);
	sleep(1);
	fflush(fp);

	r = cpu_stats_to_arr(fp, vals2);
	if (r < 4 || (r == EOF && ferror(fp)))
		goto fail;

	fclose(fp);

	work1 = vals1[CPU_STATE_USER] + vals1[CPU_STATE_NICE] + vals1[CPU_STATE_SYSTEM] +
		vals1[CPU_STATE_IRQ] + vals1[CPU_STATE_SOFTIRQ] + vals1[CPU_STATE_STEAL] +
		vals1[CPU_STATE_GUEST] + vals1[CPU_STATE_GUEST_NICE];
	total1 = work1 + vals1[CPU_STATE_IDLE]  + vals2[CPU_STATE_IOWAIT];

	work2 = vals2[CPU_STATE_USER] + vals2[CPU_STATE_NICE] + vals2[CPU_STATE_SYSTEM] +
		vals2[CPU_STATE_IRQ] + vals2[CPU_STATE_SOFTIRQ] + vals2[CPU_STATE_STEAL] +
		vals2[CPU_STATE_GUEST] + vals2[CPU_STATE_GUEST_NICE];
	total2 = work2 + vals2[CPU_STATE_IDLE]  + vals2[CPU_STATE_IOWAIT];

	return (double)(work2 - work1) / (total2 - total1) * 100.0;

fail:
	r = errno;
	fclose(fp);
	errno = r;
	return -1.0;
}
