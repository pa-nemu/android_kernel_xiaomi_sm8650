// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Xiaomi, Inc.
 *
 */
#include <linux/of.h>
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/mailbox_client.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include "ispv4_regops.h"

struct ispv4_mboxt_ctx {
	struct dentry *dir, *send;
	u32 send_data[4];
	struct device dev;
	struct mbox_client client;
	struct mbox_chan *chan;
	bool registered;
};

#define CTX_COUNT 16

char *ispv4_mboxt_dev_name[CTX_COUNT];
static bool ispv4_mbox_async;

static struct ispv4_mboxt_ctx contexts[CTX_COUNT];
static struct device_node *ispv4_mboxt_dt;
static struct dentry *ispv4_mboxt_debugdir;

ssize_t ispv4_mbox_test_send(struct file *fp, const char __user *data,
			     size_t count, loff_t *ppos)
{
	int ret;
	struct ispv4_mboxt_ctx *ctx = fp->private_data;
	ret = mbox_send_message(ctx->chan, ctx->send_data);
	return count;
}

static const struct file_operations ispv4_mbox_send_ops = {
	.write = ispv4_mbox_test_send,
	.open = simple_open,
	.llseek = generic_file_llseek,
};

void ispv4_mbox_test_rx_callback(struct mbox_client *cl, void *mssg)
{
	u32 *data = mssg;
	struct ispv4_mboxt_ctx *ctx =
		container_of(cl, struct ispv4_mboxt_ctx, client);
}

void ispv4_mbox_tx_done(struct mbox_client *cl, void *mssg, int r)
{
	struct ispv4_mboxt_ctx *ctx =
		container_of(cl, struct ispv4_mboxt_ctx, client);
}

void ispv4_mbox_tx_prepare(struct mbox_client *cl, void *mssg)
{
	struct ispv4_mboxt_ctx *ctx =
		container_of(cl, struct ispv4_mboxt_ctx, client);
}

static int _ctx_mgr(void *data, u64 idx, bool async)
{
	int ret = 0, i;
	struct ispv4_mboxt_ctx *ctx;

	if (idx >= CTX_COUNT) {
		return -ENOPARAM;
	}

	if (contexts[idx].registered) {
		return -ENOPARAM;
	}

	ctx = &contexts[idx];
	memset(ctx, 0, sizeof(*ctx));

	ctx->dev.init_name = ispv4_mboxt_dev_name[idx];
	ctx->dev.of_node = ispv4_mboxt_dt;
	ctx->client.dev = &ctx->dev;
	if (async) {
		ctx->client.tx_block = false;
	} else {
		ctx->client.tx_block = true;
		ctx->client.tx_tout = 1000;
	}
	ctx->client.knows_txdone = false;
	if (data != NULL)
		ctx->client.rx_callback = data;
	else
		ctx->client.rx_callback = ispv4_mbox_test_rx_callback;
	ctx->client.tx_done = ispv4_mbox_tx_done;
	ctx->client.tx_prepare = ispv4_mbox_tx_prepare;

	ctx->chan = mbox_request_channel(&ctx->client, idx);
	if (IS_ERR_OR_NULL(ctx->chan)) {
		ret = PTR_ERR(ctx->chan);
		goto err_mbox;
	}

	ctx->dir = debugfs_create_dir(ctx->dev.init_name, ispv4_mboxt_debugdir);
	if (IS_ERR_OR_NULL(ctx->dir)) {
		ret = PTR_ERR(ctx->dir);
		goto err_debug;
	}

	for (i = 0; i < 4; i++) {
		char name_buf[8];
		snprintf(name_buf, 8, "data%d", i);
		debugfs_create_u32(name_buf, 0666, ctx->dir,
				   &ctx->send_data[i]);
	}

	ctx->send = debugfs_create_file("send", 0444, ctx->dir, ctx,
					&ispv4_mbox_send_ops);
	if (IS_ERR_OR_NULL(ctx->dir)) {
		ret = PTR_ERR(ctx->dir);
		goto err_debug_send;
	}

	ctx->registered = true;
	return 0;

err_debug_send:
	debugfs_remove(ctx->dir);
err_debug:
	mbox_free_channel(ctx->chan);
err_mbox:
	return ret;
}

static int ctx_mgr(void *data, u64 idx)
{
	return _ctx_mgr(data, idx, ispv4_mbox_async);
}

__maybe_unused static int
ispv4_mbox_alloc_ctx(int idx, void (*f)(struct mbox_client *, void *),
		     bool async)
{
	return _ctx_mgr(f, idx, async);
}

static int ctx_free(void *data, u64 idx)
{
	struct ispv4_mboxt_ctx *ctx;

	if (idx >= CTX_COUNT) {
		return -ENOPARAM;
	}

	if (!contexts[idx].registered) {
		return -ENOPARAM;
	}

	ctx = &contexts[idx];
	ctx->registered = false;

	debugfs_remove(ctx->dir);
	mbox_free_channel(ctx->chan);
	return 0;
}

static int st_send_inc(void *data, u64 idx)
{
	struct ispv4_mboxt_ctx *ctx;
	int i, count, match;
	u32 s_data[4] = { 0 };

	if (idx >= CTX_COUNT) {
		return -ENOPARAM;
	}

	if (contexts[idx].registered) {
		ctx_free(NULL, idx);
	}

	if (0 != _ctx_mgr(NULL, idx, false)) {
		return 0;
	}
	ctx = &contexts[idx];
	for (i = 0; i < 100; i++) {
		s_data[0] = 0xFE000000;
		s_data[3] = i * idx;
		mbox_send_message(ctx->chan, s_data);
	}
	ctx_free(NULL, idx);
	msleep(200);
	ispv4_regops_read(0x100000, &match);
	ispv4_regops_read(0x100004, &count);

	return 0;
}

static struct completion st_recv_inc_finish;
static int st_recv_inc_count, st_recv_inc_match, st_recv_inc_idx;
void st_recv_inc_rx_callback(struct mbox_client *cl, void *mssg)
{
	u32 *s_data = mssg;
	if (s_data[3] == st_recv_inc_count * st_recv_inc_idx) {
		st_recv_inc_match++;
	}
	st_recv_inc_count++;
	if (st_recv_inc_count == 100) {
		complete(&st_recv_inc_finish);
	}
}

static int st_recv_inc(void *data, u64 idx)
{
	if (idx >= CTX_COUNT) {
		return -ENOPARAM;
	}

	if (contexts[idx].registered) {
		ctx_free(NULL, idx);
	}

	if (0 != _ctx_mgr(st_recv_inc_rx_callback, idx, false)) {
		return 0;
	}
	init_completion(&st_recv_inc_finish);
	st_recv_inc_count = 0;
	st_recv_inc_match = 0;
	st_recv_inc_idx = idx;
	wait_for_completion_timeout(&st_recv_inc_finish,
				    msecs_to_jiffies(1000 * 30));
	ctx_free(NULL, idx);

	return 0;
}

static int st_loopback_match;
static u32 st_loopback_rand;
void st_loopback_rx_callback(struct mbox_client *cl, void *mssg)
{
	u32 *s_data = mssg;
	smp_rmb();
	if (s_data[3] == st_loopback_rand) {
		st_loopback_match++;
		smp_wmb();
	}
}

static int st_loopback(void *data, u64 idx)
{
	u32 s_data[4] = { 0 };
	int count;
	struct ispv4_mboxt_ctx *ctx;

	if (idx >= CTX_COUNT) {
		return -ENOPARAM;
	}

	if (contexts[idx].registered) {
		ctx_free(NULL, idx);
	}

	if (0 != _ctx_mgr(st_loopback_rx_callback, idx, false)) {
		return 0;
	}
	st_loopback_match = 0;
	ctx = &contexts[idx];
	for (count = 0; count < 32; count++) {
		s_data[0] = 0xFD000000;
		s_data[3] = get_random_u32();
		st_loopback_rand = s_data[3];
		smp_wmb();
		mbox_send_message(ctx->chan, s_data);
		msleep(1000);
	}
	ctx_free(NULL, idx);

	return 0;
}

static struct completion st_iram_recv_finish;
static int st_iram_recv_match;
static int st_iram_recv_count;
void st_iram_recv_rx_callback(struct mbox_client *cl, void *mssg)
{
	u32 *data = mssg;
	u32 i, val, ret;
	for (i = 0; i < data[2]; i++) {
		ret = ispv4_regops_read(data[1] + 4 * i, &val);
		if (ret != 0 || data[3] != val)
			goto out;
	}
	st_iram_recv_match++;
out:
	st_iram_recv_count++;
	if (st_iram_recv_count == 100)
		complete(&st_iram_recv_finish);
}

static int st_iram_recv(void *data, u64 idx)
{
	if (idx >= CTX_COUNT) {
		return -ENOPARAM;
	}

	if (contexts[idx].registered) {
		ctx_free(NULL, idx);
	}

	if (0 != _ctx_mgr(st_iram_recv_rx_callback, idx, false)) {
		return 0;
	}
	st_iram_recv_match = 0;
	st_iram_recv_count = 0;
	init_completion(&st_iram_recv_finish);
	wait_for_completion_timeout(&st_iram_recv_finish,
				    msecs_to_jiffies(1000 * 90));
	ctx_free(NULL, idx);

	return 0;
}

static int st_iram_send(void *data, u64 idx)
{
	struct ispv4_mboxt_ctx *ctx;
	int i, count, match, j;
	u32 s_data[4] = { 0 };

	if (idx >= CTX_COUNT) {
		return -ENOPARAM;
	}

	if (contexts[idx].registered) {
		ctx_free(NULL, idx);
	}

	if (0 != _ctx_mgr(NULL, idx, false)) {
		return 0;
	}

#define IRAM_SEND_MAX_LEN 16
#define IRAM_SEND_START_ADDR 0x100000
#define IRAM_SEND_END_ADDR 0x100f00
#define IRAM_SEND_SIZE (IRAM_SEND_END_ADDR - IRAM_SEND_START_ADDR)

	ctx = &contexts[idx];
	for (i = 0; i < 32; i++) {
		s_data[0] = 0xFC000000;
		s_data[1] = IRAM_SEND_START_ADDR +
			    ((get_random_u32() % IRAM_SEND_SIZE) & ~0x3);
		s_data[2] = get_random_u32() % IRAM_SEND_MAX_LEN;
		s_data[3] = get_random_u32();
		for (j = 0; j < s_data[2]; j++) {
			ispv4_regops_write(s_data[1] + j * 4, s_data[3]);
		}
		mbox_send_message(ctx->chan, s_data);
		msleep(1000);
	}
	ctx_free(NULL, idx);
	msleep(200);
	ispv4_regops_read(0x100000, &match);
	ispv4_regops_read(0x100004, &count);

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(ctx_fops, NULL, ctx_mgr, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(ctx_free_fops, NULL, ctx_free, "%llu\n");

DEFINE_DEBUGFS_ATTRIBUTE(st_send_inc_fops, NULL, st_send_inc, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(st_recv_inc_fops, NULL, st_recv_inc, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(st_loopback_fops, NULL, st_loopback, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(st_iram_recv_fops, NULL, st_iram_recv, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(st_iram_send_fops, NULL, st_iram_send, "%llu\n");

static int __init ispv4_mbox_test_init(void)
{
	int ret, i;
	struct dentry *tfile;
	ispv4_mboxt_dt = of_find_node_by_name(NULL, "xm_ispv4_mbox_test");
	if (ispv4_mboxt_dt == NULL) {
		ret = -EINVAL;
		goto err_dt;
	}

	ispv4_mboxt_debugdir =
		debugfs_create_dir("ispv4_mbox_test", NULL);
	if (IS_ERR_OR_NULL(ispv4_mboxt_debugdir)) {
		ret = -EINVAL;
		goto err_debug;
	}

	tfile = debugfs_create_file("ctx_mgr", 0222, ispv4_mboxt_debugdir, NULL,
				    &ctx_fops);
	if (IS_ERR_OR_NULL(tfile)) {
		ret = -EINVAL;
		goto err_debug_f;
	}

	tfile = debugfs_create_file("ctx_free", 0222, ispv4_mboxt_debugdir,
				    NULL, &ctx_free_fops);
	if (IS_ERR_OR_NULL(tfile)) {
		ret = -EINVAL;
		goto err_debug_f;
	}

	tfile = debugfs_create_file("st_send_inc", 0222, ispv4_mboxt_debugdir,
				    NULL, &st_send_inc_fops);
	if (IS_ERR_OR_NULL(tfile)) {
		ret = -EINVAL;
		goto err_debug_f;
	}

	tfile = debugfs_create_file("st_recv_inc", 0222, ispv4_mboxt_debugdir,
				    NULL, &st_recv_inc_fops);
	if (IS_ERR_OR_NULL(tfile)) {
		ret = -EINVAL;
		goto err_debug_f;
	}

	tfile = debugfs_create_file("st_loopback", 0222, ispv4_mboxt_debugdir,
				    NULL, &st_loopback_fops);
	if (IS_ERR_OR_NULL(tfile)) {
		ret = -EINVAL;
		goto err_debug_f;
	}

	tfile = debugfs_create_file("st_iram_recv", 0222, ispv4_mboxt_debugdir,
				    NULL, &st_iram_recv_fops);
	if (IS_ERR_OR_NULL(tfile)) {
		ret = -EINVAL;
		goto err_debug_f;
	}

	tfile = debugfs_create_file("st_iram_send", 0222, ispv4_mboxt_debugdir,
				    NULL, &st_iram_send_fops);
	if (IS_ERR_OR_NULL(tfile)) {
		ret = -EINVAL;
		goto err_debug_f;
	}

	debugfs_create_bool("async", 0666, ispv4_mboxt_debugdir,
				    &ispv4_mbox_async);

	for (i = 0; i < CTX_COUNT; i++) {
		ispv4_mboxt_dev_name[i] = kasprintf(GFP_KERNEL, "mbox-%d", i);
	}

	return 0;

err_debug_f:
	debugfs_remove(ispv4_mboxt_debugdir);
err_debug:
	of_node_put(ispv4_mboxt_dt);
err_dt:
	return ret;
}

static void __exit ispv4_mbox_test_exit(void)
{
	int i;

	for (i = 0; i < CTX_COUNT; i++) {
		char *tmp = ispv4_mboxt_dev_name[i];
		if (tmp != NULL)
			kfree(tmp);
	}
	debugfs_remove(ispv4_mboxt_debugdir);
	for (i = 0; i < CTX_COUNT; i++) {
		if (contexts[i].registered) {
			mbox_free_channel(contexts[i].chan);
		}
	}
	of_node_put(ispv4_mboxt_dt);
}

module_init(ispv4_mbox_test_init);
module_exit(ispv4_mbox_test_exit);

MODULE_AUTHOR("Chenhonglin <chenhonglin@xiaomi.com>");
MODULE_DESCRIPTION("Xiaomi ISPV4 mailbox driver test");
MODULE_LICENSE("GPL v2");
