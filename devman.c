#include "devman.h"

#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <limits.h>
#include <mntent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
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

double total_mem_usage(const struct devman_ctx *ctx, bool swap) {
	assert(ctx);
	
	if (swap)
		return (double) (ctx->sysinfo->totalswap - ctx->sysinfo->freeswap)
			/ ctx->sysinfo->totalswap * 100.0;
	return (double) (ctx->sysinfo->totalram - ctx->sysinfo->freeram)
			/ ctx->sysinfo->totalram * 100.0;
}
