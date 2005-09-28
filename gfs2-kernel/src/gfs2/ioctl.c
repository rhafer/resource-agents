/*
 * Copyright (C) Sistina Software, Inc.  1997-2003 All rights reserved.
 * Copyright (C) 2004-2005 Red Hat, Inc.  All rights reserved.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 */

#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/completion.h>
#include <linux/buffer_head.h>
#include <linux/gfs2_ioctl.h>
#include <asm/semaphore.h>
#include <asm/uaccess.h>

#include "gfs2.h"
#include "bmap.h"
#include "dir.h"
#include "eattr.h"
#include "glock.h"
#include "glops.h"
#include "inode.h"
#include "ioctl.h"
#include "jdata.h"
#include "log.h"
#include "meta_io.h"
#include "quota.h"
#include "resize.h"
#include "rgrp.h"
#include "super.h"
#include "trans.h"

#define ARG_SIZE 32

/**
 * gi_get_super - Return the "struct gfs2_sb" for a filesystem
 * @sdp:
 * @gi:
 *
 * Returns: errno
 */

static int gi_get_super(struct gfs2_sbd *sdp, struct gfs2_ioctl *gi)
{
	struct gfs2_holder sb_gh;
	struct buffer_head *bh;
	struct gfs2_sb *sb;
	int error;

	if (gi->gi_argc != 1)
		return -EINVAL;
	if (gi->gi_size != sizeof(struct gfs2_sb))
		return -EINVAL;

	sb = kmalloc(sizeof(struct gfs2_sb), GFP_KERNEL);
	if (!sb)
		return -ENOMEM;

	error = gfs2_glock_nq_num(sdp,
				 GFS2_SB_LOCK, &gfs2_meta_glops,
				 LM_ST_SHARED, 0, &sb_gh);
	if (error)
		goto out;

	error = gfs2_meta_read(sb_gh.gh_gl,
			       GFS2_SB_ADDR >> sdp->sd_fsb2bb_shift,
			       DIO_START | DIO_WAIT,
			       &bh);
	if (error) {
		gfs2_glock_dq_uninit(&sb_gh);
		goto out;
	}
	gfs2_sb_in(sb, bh->b_data);
	brelse(bh);

	gfs2_glock_dq_uninit(&sb_gh);

	if (copy_to_user(gi->gi_data, sb, sizeof(struct gfs2_sb)))
		error = -EFAULT;
	else
		error = sizeof(struct gfs2_sb);

 out:
	kfree(sb);

	return error;
}

static int gi_get_file_stat(struct gfs2_inode *ip, struct gfs2_ioctl *gi)
{
	struct gfs2_holder i_gh;
	struct gfs2_dinode *di;
	int error;

	if (gi->gi_argc != 1)
		return -EINVAL;
	if (gi->gi_size != sizeof(struct gfs2_dinode))
		return -EINVAL;

	di = kmalloc(sizeof(struct gfs2_dinode), GFP_KERNEL);
	if (!di)
		return -ENOMEM;

	error = gfs2_glock_nq_init(ip->i_gl, LM_ST_SHARED, LM_FLAG_ANY, &i_gh);
	if (error)
		goto out;
	memcpy(di, &ip->i_di, sizeof(struct gfs2_dinode));
	gfs2_glock_dq_uninit(&i_gh);

	if (copy_to_user(gi->gi_data, di, sizeof(struct gfs2_dinode)))
		error = -EFAULT;
	else
		error = sizeof(struct gfs2_dinode);

 out:
	kfree(di);

	return error;
}

static int gi_set_file_flag(struct gfs2_inode *ip, struct gfs2_ioctl *gi)
{
	char buf[ARG_SIZE];
	int set;
	uint32_t flag;
	struct gfs2_holder i_gh;
	struct buffer_head *dibh;
	int error;

	if (gi->gi_argc != 3)
		return -EINVAL;

	if (strncpy_from_user(buf, gi->gi_argv[1], ARG_SIZE) < 0)
		return -EFAULT;
	buf[ARG_SIZE - 1] = 0;

	if (strcmp(buf, "set") == 0)
		set = 1;
	else if (strcmp(buf, "clear") == 0)
		set = 0;
	else
		return -EINVAL;

	if (strncpy_from_user(buf, gi->gi_argv[2], ARG_SIZE) < 0)
		return -EFAULT;
	buf[ARG_SIZE - 1] = 0;

	error = gfs2_glock_nq_init(ip->i_gl, LM_ST_EXCLUSIVE, 0, &i_gh);
	if (error)
		return error;

	error = -EACCES;
	if (ip->i_di.di_uid != current->fsuid && !capable(CAP_FOWNER))
		goto out;

	error = -EINVAL;

	if (strcmp(buf, "jdata") == 0) {
		if (!S_ISREG(ip->i_di.di_mode) || ip->i_di.di_size)
			goto out;
		flag = GFS2_DIF_JDATA;
	} else if (strcmp(buf, "directio") == 0) {
		if (!S_ISREG(ip->i_di.di_mode))
			goto out;
		flag = GFS2_DIF_DIRECTIO;
	} else if (strcmp(buf, "immutable") == 0) {
		/* The IMMUTABLE flag can only be changed by
		   the relevant capability. */
		error = -EPERM;
		if (!capable(CAP_LINUX_IMMUTABLE))
			goto out;
		flag = GFS2_DIF_IMMUTABLE;
	} else if (strcmp(buf, "appendonly") == 0) {
		/* The APPENDONLY flag can only be changed by
		   the relevant capability. */
		error = -EPERM;
		if (!capable(CAP_LINUX_IMMUTABLE))
			goto out;
		flag = GFS2_DIF_APPENDONLY;
	} else if (strcmp(buf, "inherit_jdata") == 0) {
		if (!S_ISDIR(ip->i_di.di_mode))
			goto out;
		flag = GFS2_DIF_INHERIT_JDATA;
	} else if (strcmp(buf, "inherit_directio") == 0) {
		if (S_ISDIR(ip->i_di.di_mode))
			goto out;
		flag = GFS2_DIF_INHERIT_DIRECTIO;
	} else
		goto out;

	error = gfs2_trans_begin(ip->i_sbd, RES_DINODE, 0);
	if (error)
		goto out;

	error = gfs2_meta_inode_buffer(ip, &dibh);
	if (error)
		goto out_trans_end;

	if (set)
		ip->i_di.di_flags |= flag;
	else
		ip->i_di.di_flags &= ~flag;

	gfs2_trans_add_bh(ip->i_gl, dibh);
	gfs2_dinode_out(&ip->i_di, dibh->b_data);

	brelse(dibh);

 out_trans_end:
	gfs2_trans_end(ip->i_sbd);

 out:
	gfs2_glock_dq_uninit(&i_gh);

	return error;

}

static int gi_get_bmap(struct gfs2_inode *ip, struct gfs2_ioctl *gi)
{
	struct gfs2_holder gh;
	uint64_t lblock, dblock = 0;
	int new = 0;
	int error;

	if (gi->gi_argc != 1)
		return -EINVAL;
	if (gi->gi_size != sizeof(uint64_t))
		return -EINVAL;

	error = copy_from_user(&lblock, gi->gi_data, sizeof(uint64_t));
	if (error)
		return -EFAULT;

	error = gfs2_glock_nq_init(ip->i_gl, LM_ST_SHARED, LM_FLAG_ANY, &gh);
	if (error)
		return error;

	error = -EACCES;
	if (ip->i_di.di_uid == current->fsuid || capable(CAP_FOWNER)) {
		error = 0;
		if (!gfs2_is_stuffed(ip))
			error = gfs2_block_map(ip, lblock, &new, &dblock, NULL);
	}

	gfs2_glock_dq_uninit(&gh);

	if (!error) {
		error = copy_to_user(gi->gi_data, &dblock, sizeof(uint64_t));
		if (error)
			error = -EFAULT;
	}

	return error;
}

/**
 * gi_get_file_meta - Return all the metadata for a file
 * @ip:
 * @gi:
 *
 * Returns: the number of bytes copied, or -errno
 */

static int gi_get_file_meta(struct gfs2_inode *ip, struct gfs2_ioctl *gi)
{
	struct gfs2_holder i_gh;
	struct gfs2_user_buffer ub;
	int error;

	if (gi->gi_argc != 1)
		return -EINVAL;

	ub.ub_data = gi->gi_data;
	ub.ub_size = gi->gi_size;
	ub.ub_count = 0;

	error = gfs2_glock_nq_init(ip->i_gl, LM_ST_SHARED, LM_FLAG_ANY, &i_gh);
	if (error)
		return error;

	error = -EACCES;
	if (ip->i_di.di_uid != current->fsuid && !capable(CAP_FOWNER))
		goto out;

	error = gfs2_get_file_meta(ip, &ub);
	if (error)
		goto out;

	if (S_ISDIR(ip->i_di.di_mode) &&
	    (ip->i_di.di_flags & GFS2_DIF_EXHASH)) {
		error = gfs2_get_dir_meta(ip, &ub);
		if (error)
			goto out;
	}

	if (ip->i_di.di_eattr) {
		error = gfs2_get_eattr_meta(ip, &ub);
		if (error)
			goto out;
	}

	error = ub.ub_count;

 out:
	gfs2_glock_dq_uninit(&i_gh);

	return error;
}

/**
 * gi_do_file_flush - sync out all dirty data and
 *                    drop the cache (and lock) for a file.
 * @ip:
 * @gi:
 *
 * Returns: errno
 */

static int gi_do_file_flush(struct gfs2_inode *ip, struct gfs2_ioctl *gi)
{
	if (gi->gi_argc != 1)
		return -EINVAL;
	gfs2_glock_force_drop(ip->i_gl);
	return 0;
}

/**
 * gi2hip - return the "struct gfs2_inode" for a hidden file
 * @sdp:
 * @gi:
 *
 * Returns: the "struct gfs2_inode"
 */

static struct gfs2_inode *gi2hip(struct gfs2_sbd *sdp, struct gfs2_ioctl *gi)
{
	char buf[ARG_SIZE];

	if (gi->gi_argc != 2)
		return ERR_PTR(-EINVAL);

	if (strncpy_from_user(buf, gi->gi_argv[1], ARG_SIZE) < 0)
		return ERR_PTR(-EFAULT);
	buf[ARG_SIZE - 1] = 0;

	if (strcmp(buf, "jindex") == 0)
		return sdp->sd_jindex;
	if (strcmp(buf, "rindex") == 0)
		return sdp->sd_rindex;
	if (strcmp(buf, "quota") == 0)
		return sdp->sd_quota_inode;

	return ERR_PTR(-EINVAL);
}

/**
 * gi_get_hfile_stat - get stat info on a hidden file
 * @sdp:
 * @gi:
 *
 * Returns: the number of bytes copied, or -errno
 */

static int gi_get_hfile_stat(struct gfs2_sbd *sdp, struct gfs2_ioctl *gi)
{
	struct gfs2_inode *ip;
	struct gfs2_dinode *di;
	struct gfs2_holder i_gh;
	int error;

	ip = gi2hip(sdp, gi);
	if (IS_ERR(ip))
		return PTR_ERR(ip);

	if (gi->gi_size != sizeof(struct gfs2_dinode))
		return -EINVAL;

	di = kmalloc(sizeof(struct gfs2_dinode), GFP_KERNEL);
	if (!di)
		return -ENOMEM;

	error = gfs2_glock_nq_init(ip->i_gl, LM_ST_SHARED, LM_FLAG_ANY, &i_gh);
	if (error)
		goto out;
	memcpy(di, &ip->i_di, sizeof(struct gfs2_dinode));
	gfs2_glock_dq_uninit(&i_gh);

	if (copy_to_user(gi->gi_data, di, sizeof(struct gfs2_dinode)))
		error = -EFAULT;
	else
		error = sizeof(struct gfs2_dinode);

 out:
	kfree(di);

	return error;
}

/**
 * gi_do_hfile_read - Read data from a hidden file
 * @sdp:
 * @gi:
 *
 * Returns: the number of bytes read, or -errno
 */

static int gi_do_hfile_read(struct gfs2_sbd *sdp, struct gfs2_ioctl *gi)
{
	struct gfs2_inode *ip;
	struct gfs2_holder i_gh;
	int error;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	ip = gi2hip(sdp, gi);
	if (IS_ERR(ip))
		return PTR_ERR(ip);

	if (!S_ISREG(ip->i_di.di_mode))
		return -EINVAL;

	if (!access_ok(VERIFY_WRITE, gi->gi_data, gi->gi_size))
		return -EFAULT;

	error = gfs2_glock_nq_init(ip->i_gl, LM_ST_SHARED, 0, &i_gh);
	if (error)
		return error;

	error = gfs2_jdata_read(ip, gi->gi_data, gi->gi_offset, gi->gi_size,
				gfs2_copy2user);

	gfs2_glock_dq_uninit(&i_gh);

	return error;
}

/**
 * gi_do_hfile_write - Write data to a hidden file
 * @sdp:
 * @gi:
 *
 * Returns: the number of bytes written, or -errno
 */

static int gi_do_hfile_write(struct gfs2_sbd *sdp, struct gfs2_ioctl *gi)
{
	struct gfs2_inode *ip;
	struct gfs2_alloc *al = NULL;
	struct gfs2_holder i_gh;
	unsigned int data_blocks, ind_blocks;
	int alloc_required;
	int error;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	ip = gi2hip(sdp, gi);
	if (IS_ERR(ip))
		return PTR_ERR(ip);

	if (!S_ISREG(ip->i_di.di_mode))
		return -EINVAL;

	if (!access_ok(VERIFY_READ, gi->gi_data, gi->gi_size))
		return -EFAULT;

	gfs2_write_calc_reserv(ip, gi->gi_size, &data_blocks, &ind_blocks);

	error = gfs2_glock_nq_init(ip->i_gl, LM_ST_EXCLUSIVE,
				   LM_FLAG_PRIORITY, &i_gh);
	if (error)
		return error;

	if (!gfs2_is_jdata(ip)) {
		gfs2_consist_inode(ip);
		error = -EIO;
		goto out;
	}

	error = gfs2_write_alloc_required(ip, gi->gi_offset, gi->gi_size,
					  &alloc_required);
	if (error)
		goto out;

	if (alloc_required) {
		al = gfs2_alloc_get(ip);

		al->al_requested = data_blocks + ind_blocks;

		error = gfs2_inplace_reserve(ip);
		if (error)
			goto out_alloc;

		error = gfs2_trans_begin(sdp,
					 al->al_rgd->rd_ri.ri_length +
					 data_blocks + ind_blocks +
					 RES_DINODE + RES_STATFS, 0);
		if (error)
			goto out_relse;
	} else {
		error = gfs2_trans_begin(sdp, data_blocks + RES_DINODE, 0);
		if (error)
			goto out;
	}

	error = gfs2_jdata_write(ip, gi->gi_data, gi->gi_offset, gi->gi_size,
				 gfs2_copy_from_user);

	gfs2_trans_end(sdp);

 out_relse:
	if (alloc_required)
		gfs2_inplace_release(ip);

 out_alloc:
	if (alloc_required)
		gfs2_alloc_put(ip);

 out:
	gfs2_glock_dq_uninit(&i_gh);

	return error;
}

/**
 * gi_do_hfile_trunc - truncate a hidden file
 * @sdp:
 * @gi:
 *
 * Returns: the number of bytes copied, or -errno
 */

static int gi_do_hfile_trunc(struct gfs2_sbd *sdp, struct gfs2_ioctl *gi)
{
	struct gfs2_inode *ip;
	struct gfs2_holder i_gh;
	int error;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	ip = gi2hip(sdp, gi);
	if (IS_ERR(ip))
		return PTR_ERR(ip);

	if (!S_ISREG(ip->i_di.di_mode))
		return -EINVAL;

	error = gfs2_glock_nq_init(ip->i_gl, LM_ST_EXCLUSIVE, 0, &i_gh);
	if (error)
		return error;

	error = gfs2_truncatei(ip, gi->gi_offset, NULL);

	gfs2_glock_dq_uninit(&i_gh);

	return error;
}

/**
 * gi_do_quota_sync - sync the outstanding quota changes for a FS
 * @sdp:
 * @gi:
 *
 * Returns: errno
 */

static int gi_do_quota_sync(struct gfs2_sbd *sdp, struct gfs2_ioctl *gi)
{
	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;
	if (gi->gi_argc != 1)
		return -EINVAL;
	return gfs2_quota_sync(sdp);
}

/**
 * gi_do_quota_refresh - Refresh the a quota LVB from the quota file
 * @sdp:
 * @gi:
 *
 * Returns: errno
 */

static int gi_do_quota_refresh(struct gfs2_sbd *sdp, struct gfs2_ioctl *gi)
{
	char buf[ARG_SIZE];
	int user;
	uint32_t id;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;
	if (gi->gi_argc != 2)
		return -EINVAL;

	if (strncpy_from_user(buf, gi->gi_argv[1], ARG_SIZE) < 0)
		return -EFAULT;
	buf[ARG_SIZE - 1] = 0;

	switch (buf[0]) {
	case 'u':
		user = 1;
		break;
	case 'g':
		user = 0;
		break;
	default:
		return -EINVAL;
	}

	if (buf[1] != ':')
		return -EINVAL;

	if (sscanf(buf + 2, "%u", &id) != 1)
		return -EINVAL;

	return gfs2_quota_refresh(sdp, user, id);
}

/**
 * gi_do_quota_read - read quota values from the quota file
 * @sdp:
 * @gi:
 *
 * Returns: errno
 */

static int gi_do_quota_read(struct gfs2_sbd *sdp, struct gfs2_ioctl *gi)
{
	char buf[ARG_SIZE];
	int user;
	uint32_t id;
	struct gfs2_quota q;
	int error;

	if (gi->gi_argc != 2)
		return -EINVAL;
	if (gi->gi_size != sizeof(struct gfs2_quota))
		return -EINVAL;

	if (strncpy_from_user(buf, gi->gi_argv[1], ARG_SIZE) < 0)
		return -EFAULT;
	buf[ARG_SIZE - 1] = 0;

	switch (buf[0]) {
	case 'u':
		user = 1;
		break;
	case 'g':
		user = 0;
		break;
	default:
		return -EINVAL;
	}

	if (buf[1] != ':')
		return -EINVAL;

	if (sscanf(buf + 2, "%u", &id) != 1)
		return -EINVAL;

	error = gfs2_quota_read(sdp, user, id, &q);
	if (error)
		return error;

	if (copy_to_user(gi->gi_data, &q, sizeof(struct gfs2_quota)))
		return -EFAULT;

	return 0;
}

static int gi_resize_add_rgrps(struct gfs2_sbd *sdp, struct gfs2_ioctl *gi)
{
	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;
	if (gi->gi_argc != 1)
		return -EINVAL;
	if (gi->gi_size % sizeof(struct gfs2_rindex))
		return -EINVAL;

	return gfs2_resize_add_rgrps(sdp, gi->gi_data, gi->gi_size);
}

static int gi_rename2system(struct gfs2_sbd *sdp, struct gfs2_ioctl *gi)
{
	char new_dir[ARG_SIZE], new_name[ARG_SIZE];
	struct gfs2_inode *old_dip, *ip, *new_dip;
	int put_new_dip = 0;
	int error;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;
	if (gi->gi_argc != 3)
		return -EINVAL;

	if (strncpy_from_user(new_dir, gi->gi_argv[1], ARG_SIZE) < 0)
		return -EFAULT;
	new_dir[ARG_SIZE - 1] = 0;
	if (strncpy_from_user(new_name, gi->gi_argv[2], ARG_SIZE) < 0)
		return -EFAULT;
	new_name[ARG_SIZE - 1] = 0;

	error = gfs2_lookup_simple(sdp->sd_root_dir, ".gfs2_admin", &old_dip);
	if (error)
		return error;

	error = -ENOTDIR;
	if (!S_ISDIR(old_dip->i_di.di_mode))
		goto out;

	error = gfs2_lookup_simple(old_dip, "new_inode", &ip);
	if (error)
		goto out;

	if (!strcmp(new_dir, "per_node")) {
		error = gfs2_lookup_simple(sdp->sd_master_dir, "per_node",
					   &new_dip);
		if (error)
			goto out2;
		put_new_dip = 1;
	} else if (!strcmp(new_dir, "jindex"))
		new_dip = sdp->sd_jindex;
	else {
		error = -EINVAL;
		goto out2;
	}

	error = gfs2_rename2system(ip, old_dip, "new_inode", new_dip, new_name);

	if (put_new_dip)
		gfs2_inode_put(new_dip);

 out2:
	gfs2_inode_put(ip);
	
 out:
	gfs2_inode_put(old_dip);

	return error;
}

int gfs2_ioctl_i(struct gfs2_inode *ip, void *arg)
{
	struct gfs2_ioctl *gi_user = (struct gfs2_ioctl *)arg;
	struct gfs2_ioctl gi;
	char **argv;
	char arg0[ARG_SIZE];
	int error = -EFAULT;

	if (copy_from_user(&gi, gi_user, sizeof(struct gfs2_ioctl)))
		return -EFAULT;
	if (!gi.gi_argc)
		return -EINVAL;
	argv = kcalloc(gi.gi_argc, sizeof(char *), GFP_KERNEL);
	if (!argv)
		return -ENOMEM;
	if (copy_from_user(argv, gi.gi_argv, gi.gi_argc * sizeof(char *)))
		goto out;
	gi.gi_argv = argv;

	if (strncpy_from_user(arg0, argv[0], ARG_SIZE) < 0)
		goto out;
	arg0[ARG_SIZE - 1] = 0;

	if (strcmp(arg0, "get_super") == 0)
		error = gi_get_super(ip->i_sbd, &gi);
	else if (strcmp(arg0, "get_file_stat") == 0)
		error = gi_get_file_stat(ip, &gi);
	else if (strcmp(arg0, "set_file_flag") == 0)
		error = gi_set_file_flag(ip, &gi);
	else if (strcmp(arg0, "get_bmap") == 0)
		error = gi_get_bmap(ip, &gi);
	else if (strcmp(arg0, "get_file_meta") == 0)
		error = gi_get_file_meta(ip, &gi);
	else if (strcmp(arg0, "do_file_flush") == 0)
		error = gi_do_file_flush(ip, &gi);
	else if (strcmp(arg0, "get_hfile_stat") == 0)
		error = gi_get_hfile_stat(ip->i_sbd, &gi);
	else if (strcmp(arg0, "do_hfile_read") == 0)
		error = gi_do_hfile_read(ip->i_sbd, &gi);
	else if (strcmp(arg0, "do_hfile_write") == 0)
		error = gi_do_hfile_write(ip->i_sbd, &gi);
	else if (strcmp(arg0, "do_hfile_trunc") == 0)
		error = gi_do_hfile_trunc(ip->i_sbd, &gi);
	else if (strcmp(arg0, "do_quota_sync") == 0)
		error = gi_do_quota_sync(ip->i_sbd, &gi);
	else if (strcmp(arg0, "do_quota_refresh") == 0)
		error = gi_do_quota_refresh(ip->i_sbd, &gi);
	else if (strcmp(arg0, "do_quota_read") == 0)
		error = gi_do_quota_read(ip->i_sbd, &gi);
	else if (strcmp(arg0, "resize_add_rgrps") == 0)
		error = gi_resize_add_rgrps(ip->i_sbd, &gi);
	else if (strcmp(arg0, "rename2system") == 0)
		error = gi_rename2system(ip->i_sbd, &gi);
	else
		error = -ENOTTY;

 out:
	kfree(argv);

	return error;
}

