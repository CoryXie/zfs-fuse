/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */



#include <sys/types.h>
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/uio.h>
#include <sys/buf.h>
#include <sys/modctl.h>
#include <sys/open.h>
#include <sys/file.h>
#include <sys/kmem.h>
#include <sys/conf.h>
#include <sys/cmn_err.h>
#include <sys/stat.h>
#include <sys/zfs_ioctl.h>
#include <sys/zap.h>
#include <sys/spa.h>
#include <sys/vdev.h>
#include <sys/dmu.h>
#include <sys/dsl_dir.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_prop.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/sunldi.h>
#include <sys/policy.h>
#include <sys/zone.h>
#include <sys/nvpair.h>
#include <sys/pathname.h>
#include <sys/mount.h>
#include <sys/sdt.h>
#include <sys/fs/zfs.h>
#include <sys/zfs_ctldir.h>
#include <sys/zvol.h>

#include "zfs_namecheck.h"
#include "zfs_prop.h"

extern struct modlfs zfs_modlfs;

extern void zfs_init(void);
extern void zfs_fini(void);

ldi_ident_t zfs_li = NULL;
dev_info_t *zfs_dip;

typedef int zfs_ioc_func_t(zfs_cmd_t *);
typedef int zfs_secpolicy_func_t(const char *, cred_t *);

typedef struct zfs_ioc_vec {
	zfs_ioc_func_t		*zvec_func;
	zfs_secpolicy_func_t	*zvec_secpolicy;
	enum {
		no_name,
		pool_name,
		dataset_name
	}			zvec_namecheck;
} zfs_ioc_vec_t;

/* _NOTE(PRINTFLIKE(4)) - this is printf-like, but lint is too whiney */
void
__dprintf(const char *file, const char *func, int line, const char *fmt, ...)
{
	const char *newfile;
	char buf[256];
	va_list adx;

	/*
	 * Get rid of annoying "../common/" prefix to filename.
	 */
	newfile = strrchr(file, '/');
	if (newfile != NULL) {
		newfile = newfile + 1; /* Get rid of leading / */
	} else {
		newfile = file;
	}

	va_start(adx, fmt);
	(void) vsnprintf(buf, sizeof (buf), fmt, adx);
	va_end(adx);

// 	fprintf(stderr, "%s", buf);

	/*
	 * To get this data, use the zfs-dprintf probe as so:
	 * dtrace -q -n 'zfs-dprintf \
	 *	/stringof(arg0) == "dbuf.c"/ \
	 *	{printf("%s: %s", stringof(arg1), stringof(arg3))}'
	 * arg0 = file name
	 * arg1 = function name
	 * arg2 = line number
	 * arg3 = message
	 */
	DTRACE_PROBE4(zfs__dprintf,
	    char *, newfile, char *, func, int, line, char *, buf);
}

/*
 * Policy for top-level read operations (list pools).  Requires no privileges,
 * and can be used in the local zone, as there is no associated dataset.
 */
/* ARGSUSED */
static int
zfs_secpolicy_none(const char *unused1, cred_t *cr)
{
	return (0);
}

/*
 * Policy for dataset read operations (list children, get statistics).  Requires
 * no privileges, but must be visible in the local zone.
 */
/* ARGSUSED */
static int
zfs_secpolicy_read(const char *dataset, cred_t *cr)
{
	if (INGLOBALZONE(curproc) ||
	    zone_dataset_visible(dataset, NULL))
		return (0);

	return (ENOENT);
}

static int
zfs_dozonecheck(const char *dataset, cred_t *cr)
{
	uint64_t zoned;
	int writable = 1;

	/*
	 * The dataset must be visible by this zone -- check this first
	 * so they don't see EPERM on something they shouldn't know about.
	 */
	if (!INGLOBALZONE(curproc) &&
	    !zone_dataset_visible(dataset, &writable))
		return (ENOENT);

	if (dsl_prop_get_integer(dataset, "zoned", &zoned, NULL))
		return (ENOENT);

	if (INGLOBALZONE(curproc)) {
		/*
		 * If the fs is zoned, only root can access it from the
		 * global zone.
		 */
		if (secpolicy_zfs(cr) && zoned)
			return (EPERM);
	} else {
		/*
		 * If we are in a local zone, the 'zoned' property must be set.
		 */
		if (!zoned)
			return (EPERM);

		/* must be writable by this zone */
		if (!writable)
			return (EPERM);
	}
	return (0);
}

/*
 * Policy for dataset write operations (create children, set properties, etc).
 * Requires SYS_MOUNT privilege, and must be writable in the local zone.
 */
int
zfs_secpolicy_write(const char *dataset, cred_t *cr)
{
	int error;

	if (error = zfs_dozonecheck(dataset, cr))
		return (error);

	return (secpolicy_zfs(cr));
}

/*
 * Policy for operations that want to write a dataset's parent:
 * create, destroy, snapshot, clone, restore.
 */
static int
zfs_secpolicy_parent(const char *dataset, cred_t *cr)
{
	char parentname[MAXNAMELEN];
	char *cp;

	/*
	 * Remove the @bla or /bla from the end of the name to get the parent.
	 */
	(void) strncpy(parentname, dataset, sizeof (parentname));
	cp = strrchr(parentname, '@');
	if (cp != NULL) {
		cp[0] = '\0';
	} else {
		cp = strrchr(parentname, '/');
		if (cp == NULL)
			return (ENOENT);
		cp[0] = '\0';

	}

	return (zfs_secpolicy_write(parentname, cr));
}

/*
 * Policy for pool operations - create/destroy pools, add vdevs, etc.  Requires
 * SYS_CONFIG privilege, which is not available in a local zone.
 */
/* ARGSUSED */
static int
zfs_secpolicy_config(const char *unused, cred_t *cr)
{
	if (secpolicy_sys_config(cr, B_FALSE) != 0)
		return (EPERM);

	return (0);
}

/*
 * Policy for fault injection.  Requires all privileges.
 */
/* ARGSUSED */
static int
zfs_secpolicy_inject(const char *unused, cred_t *cr)
{
	return (secpolicy_zinject(cr));
}

/*
 * Returns the nvlist as specified by the user in the zfs_cmd_t.
 */
static int
get_nvlist(zfs_cmd_t *zc, nvlist_t **nvp)
{
	char *packed;
	size_t size;
	int error;
	nvlist_t *config = NULL;

	/*
	 * Read in and unpack the user-supplied nvlist.
	 */
	if ((size = zc->zc_nvlist_src_size) == 0)
		return (EINVAL);

	packed = kmem_alloc(size, KM_SLEEP);

	if ((error = xcopyin((void *)(uintptr_t)zc->zc_nvlist_src, packed,
	    size)) != 0) {
		kmem_free(packed, size);
		return (error);
	}

	if ((error = nvlist_unpack(packed, size, &config, 0)) != 0) {
		kmem_free(packed, size);
		return (error);
	}

	kmem_free(packed, size);

	*nvp = config;
	return (0);
}

static int
put_nvlist(zfs_cmd_t *zc, nvlist_t *nvl)
{
	char *packed = NULL;
	size_t size;
	int error;

	VERIFY(nvlist_size(nvl, &size, NV_ENCODE_NATIVE) == 0);

	if (size > zc->zc_nvlist_dst_size) {
		error = ENOMEM;
	} else {
		VERIFY(nvlist_pack(nvl, &packed, &size, NV_ENCODE_NATIVE,
		    KM_SLEEP) == 0);
		error = xcopyout(packed, (void *)(uintptr_t)zc->zc_nvlist_dst,
		    size);
		kmem_free(packed, size);
	}

	zc->zc_nvlist_dst_size = size;
	return (error);
}

static int
zfs_ioc_pool_create(zfs_cmd_t *zc)
{
	int error;
	nvlist_t *config;

	if ((error = get_nvlist(zc, &config)) != 0)
		return (error);

	error = spa_create(zc->zc_name, config, zc->zc_value[0] == '\0' ?
	    NULL : zc->zc_value);

	nvlist_free(config);

	return (error);
}

static int
zfs_ioc_pool_destroy(zfs_cmd_t *zc)
{
	return (spa_destroy(zc->zc_name));
}

static int
zfs_ioc_pool_import(zfs_cmd_t *zc)
{
	int error;
	nvlist_t *config;
	uint64_t guid;

	if ((error = get_nvlist(zc, &config)) != 0)
		return (error);

	if (nvlist_lookup_uint64(config, ZPOOL_CONFIG_POOL_GUID, &guid) != 0 ||
	    guid != zc->zc_guid)
		error = EINVAL;
	else
		error = spa_import(zc->zc_name, config,
		    zc->zc_value[0] == '\0' ? NULL : zc->zc_value);

	nvlist_free(config);

	return (error);
}

static int
zfs_ioc_pool_export(zfs_cmd_t *zc)
{
	return (spa_export(zc->zc_name, NULL));
}

static int
zfs_ioc_pool_configs(zfs_cmd_t *zc)
{
	nvlist_t *configs;
	int error;

	if ((configs = spa_all_configs(&zc->zc_cookie)) == NULL)
		return (EEXIST);

	error = put_nvlist(zc, configs);

	nvlist_free(configs);

	return (error);
}

static int
zfs_ioc_pool_stats(zfs_cmd_t *zc)
{
	nvlist_t *config;
	int error;
	int ret = 0;

	error = spa_get_stats(zc->zc_name, &config, zc->zc_value,
	    sizeof (zc->zc_value));

	if (config != NULL) {
		ret = put_nvlist(zc, config);
		nvlist_free(config);

		/*
		 * The config may be present even if 'error' is non-zero.
		 * In this case we return success, and preserve the real errno
		 * in 'zc_cookie'.
		 */
		zc->zc_cookie = error;
	} else {
		ret = error;
	}

	return (ret);
}

/*
 * Try to import the given pool, returning pool stats as appropriate so that
 * user land knows which devices are available and overall pool health.
 */
static int
zfs_ioc_pool_tryimport(zfs_cmd_t *zc)
{
	nvlist_t *tryconfig, *config;
	int error;

	if ((error = get_nvlist(zc, &tryconfig)) != 0)
		return (error);

	config = spa_tryimport(tryconfig);

	nvlist_free(tryconfig);

	if (config == NULL)
		return (EINVAL);

	error = put_nvlist(zc, config);
	nvlist_free(config);

	return (error);
}

static int
zfs_ioc_pool_scrub(zfs_cmd_t *zc)
{
	spa_t *spa;
	int error;

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0)
		return (error);

	error = spa_scrub(spa, zc->zc_cookie, B_FALSE);

	spa_close(spa, FTAG);

	return (error);
}

static int
zfs_ioc_pool_freeze(zfs_cmd_t *zc)
{
	spa_t *spa;
	int error;

	error = spa_open(zc->zc_name, &spa, FTAG);
	if (error == 0) {
		spa_freeze(spa);
		spa_close(spa, FTAG);
	}
	return (error);
}

static int
zfs_ioc_pool_upgrade(zfs_cmd_t *zc)
{
	spa_t *spa;
	int error;

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0)
		return (error);

	spa_upgrade(spa);

	spa_close(spa, FTAG);

	return (error);
}

static int
zfs_ioc_pool_get_history(zfs_cmd_t *zc)
{
	spa_t *spa;
	char *hist_buf;
	uint64_t size;
	int error;

	if ((size = zc->zc_history_len) == 0)
		return (EINVAL);

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0)
		return (error);

	hist_buf = kmem_alloc(size, KM_SLEEP);
	if ((error = spa_history_get(spa, &zc->zc_history_offset,
	    &zc->zc_history_len, hist_buf)) == 0) {
		error = xcopyout(hist_buf, (char *)(uintptr_t)zc->zc_history,
		    zc->zc_history_len);
	}

	spa_close(spa, FTAG);
	kmem_free(hist_buf, size);
	return (error);
}

static int
zfs_ioc_pool_log_history(zfs_cmd_t *zc)
{
	spa_t *spa;
	char *history_str = NULL;
	size_t size;
	int error;

	size = zc->zc_history_len;
	if (size == 0 || size > HIS_MAX_RECORD_LEN)
		return (EINVAL);

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0)
		return (error);

	/* add one for the NULL delimiter */
	size++;
	history_str = kmem_alloc(size, KM_SLEEP);
	if ((error = xcopyin((void *)(uintptr_t)zc->zc_history, history_str,
	    size)) != 0) {
		spa_close(spa, FTAG);
		kmem_free(history_str, size);
		return (error);
	}
	history_str[size - 1] = '\0';

	error = spa_history_log(spa, history_str, zc->zc_history_offset);

	spa_close(spa, FTAG);
	kmem_free(history_str, size);

	return (error);
}

static int
zfs_ioc_vdev_add(zfs_cmd_t *zc)
{
	spa_t *spa;
	int error;
	nvlist_t *config;

	error = spa_open(zc->zc_name, &spa, FTAG);
	if (error != 0)
		return (error);

	if ((error = get_nvlist(zc, &config)) == 0) {
		error = spa_vdev_add(spa, config);
		nvlist_free(config);
	}

	spa_close(spa, FTAG);
	return (error);
}

static int
zfs_ioc_vdev_remove(zfs_cmd_t *zc)
{
	spa_t *spa;
	int error;

	error = spa_open(zc->zc_name, &spa, FTAG);
	if (error != 0)
		return (error);
	error = spa_vdev_remove(spa, zc->zc_guid, B_FALSE);
	spa_close(spa, FTAG);
	return (error);
}

static int
zfs_ioc_vdev_online(zfs_cmd_t *zc)
{
	spa_t *spa;
	int error;

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0)
		return (error);
	error = vdev_online(spa, zc->zc_guid);
	spa_close(spa, FTAG);
	return (error);
}

static int
zfs_ioc_vdev_offline(zfs_cmd_t *zc)
{
	spa_t *spa;
	int istmp = zc->zc_cookie;
	int error;

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0)
		return (error);
	error = vdev_offline(spa, zc->zc_guid, istmp);
	spa_close(spa, FTAG);
	return (error);
}

static int
zfs_ioc_vdev_attach(zfs_cmd_t *zc)
{
	spa_t *spa;
	int replacing = zc->zc_cookie;
	nvlist_t *config;
	int error;

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0)
		return (error);

	if ((error = get_nvlist(zc, &config)) == 0) {
		error = spa_vdev_attach(spa, zc->zc_guid, config, replacing);
		nvlist_free(config);
	}

	spa_close(spa, FTAG);
	return (error);
}

static int
zfs_ioc_vdev_detach(zfs_cmd_t *zc)
{
	spa_t *spa;
	int error;

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0)
		return (error);

	error = spa_vdev_detach(spa, zc->zc_guid, B_FALSE);

	spa_close(spa, FTAG);
	return (error);
}

static int
zfs_ioc_vdev_setpath(zfs_cmd_t *zc)
{
	spa_t *spa;
	char *path = zc->zc_value;
	uint64_t guid = zc->zc_guid;
	int error;

	error = spa_open(zc->zc_name, &spa, FTAG);
	if (error != 0)
		return (error);

	error = spa_vdev_setpath(spa, guid, path);
	spa_close(spa, FTAG);
	return (error);
}

static int
zfs_ioc_objset_stats(zfs_cmd_t *zc)
{
	objset_t *os = NULL;
	int error;
	nvlist_t *nv;

retry:
	error = dmu_objset_open(zc->zc_name, DMU_OST_ANY,
	    DS_MODE_STANDARD | DS_MODE_READONLY, &os);
	if (error != 0) {
		/*
		 * This is ugly: dmu_objset_open() can return EBUSY if
		 * the objset is held exclusively. Fortunately this hold is
		 * only for a short while, so we retry here.
		 * This avoids user code having to handle EBUSY,
		 * for example for a "zfs list".
		 */
		if (error == EBUSY) {
			delay(1);
			goto retry;
		}
		return (error);
	}

	dmu_objset_fast_stat(os, &zc->zc_objset_stats);

	if (zc->zc_nvlist_dst != 0 &&
	    (error = dsl_prop_get_all(os, &nv)) == 0) {
		dmu_objset_stats(os, nv);
		/*
		 * NB: zvol_get_stats() will read the objset contents,
		 * which we aren't supposed to do with a
		 * DS_MODE_STANDARD open, because it could be
		 * inconsistent.  So this is a bit of a workaround...
		 */
		if (!zc->zc_objset_stats.dds_inconsistent &&
		    dmu_objset_type(os) == DMU_OST_ZVOL)
			VERIFY(zvol_get_stats(os, nv) == 0);
		error = put_nvlist(zc, nv);
		nvlist_free(nv);
	}

	spa_altroot(dmu_objset_spa(os), zc->zc_value, sizeof (zc->zc_value));

	dmu_objset_close(os);
	return (error);
}

static int
zfs_ioc_dataset_list_next(zfs_cmd_t *zc)
{
	objset_t *os;
	int error;
	char *p;

retry:
	error = dmu_objset_open(zc->zc_name, DMU_OST_ANY,
	    DS_MODE_STANDARD | DS_MODE_READONLY, &os);
	if (error != 0) {
		/*
		 * This is ugly: dmu_objset_open() can return EBUSY if
		 * the objset is held exclusively. Fortunately this hold is
		 * only for a short while, so we retry here.
		 * This avoids user code having to handle EBUSY,
		 * for example for a "zfs list".
		 */
		if (error == EBUSY) {
			delay(1);
			goto retry;
		}
		if (error == ENOENT)
			error = ESRCH;
		return (error);
	}

	p = strrchr(zc->zc_name, '/');
	if (p == NULL || p[1] != '\0')
		(void) strlcat(zc->zc_name, "/", sizeof (zc->zc_name));
	p = zc->zc_name + strlen(zc->zc_name);

	do {
		error = dmu_dir_list_next(os,
		    sizeof (zc->zc_name) - (p - zc->zc_name), p,
		    NULL, &zc->zc_cookie);
		if (error == ENOENT)
			error = ESRCH;
	} while (error == 0 && !INGLOBALZONE(curproc) &&
	    !zone_dataset_visible(zc->zc_name, NULL));

	/*
	 * If it's a hidden dataset (ie. with a '$' in its name), don't
	 * try to get stats for it.  Userland will skip over it.
	 */
	if (error == 0 && strchr(zc->zc_name, '$') == NULL)
		error = zfs_ioc_objset_stats(zc); /* fill in the stats */

	dmu_objset_close(os);
	return (error);
}

static int
zfs_ioc_snapshot_list_next(zfs_cmd_t *zc)
{
	objset_t *os;
	int error;

retry:
	error = dmu_objset_open(zc->zc_name, DMU_OST_ANY,
	    DS_MODE_STANDARD | DS_MODE_READONLY, &os);
	if (error != 0) {
		/*
		 * This is ugly: dmu_objset_open() can return EBUSY if
		 * the objset is held exclusively. Fortunately this hold is
		 * only for a short while, so we retry here.
		 * This avoids user code having to handle EBUSY,
		 * for example for a "zfs list".
		 */
		if (error == EBUSY) {
			delay(1);
			goto retry;
		}
		if (error == ENOENT)
			error = ESRCH;
		return (error);
	}

	/*
	 * A dataset name of maximum length cannot have any snapshots,
	 * so exit immediately.
	 */
	if (strlcat(zc->zc_name, "@", sizeof (zc->zc_name)) >= MAXNAMELEN) {
		dmu_objset_close(os);
		return (ESRCH);
	}

	error = dmu_snapshot_list_next(os,
	    sizeof (zc->zc_name) - strlen(zc->zc_name),
	    zc->zc_name + strlen(zc->zc_name), NULL, &zc->zc_cookie);
	if (error == ENOENT)
		error = ESRCH;

	if (error == 0)
		error = zfs_ioc_objset_stats(zc); /* fill in the stats */

	dmu_objset_close(os);
	return (error);
}

static int
zfs_set_prop_nvlist(const char *name, dev_t dev, cred_t *cr, nvlist_t *nvl)
{
	nvpair_t *elem;
	int error;
	const char *propname;
	zfs_prop_t prop;
	uint64_t intval;
	char *strval;

	elem = NULL;
	while ((elem = nvlist_next_nvpair(nvl, elem)) != NULL) {
		propname = nvpair_name(elem);

		if ((prop = zfs_name_to_prop(propname)) ==
		    ZFS_PROP_INVAL) {
			/*
			 * If this is a user-defined property, it must be a
			 * string, and there is no further validation to do.
			 */
			if (!zfs_prop_user(propname) ||
			    nvpair_type(elem) != DATA_TYPE_STRING)
				return (EINVAL);

			VERIFY(nvpair_value_string(elem, &strval) == 0);
			error = dsl_prop_set(name, propname, 1,
			    strlen(strval) + 1, strval);
			if (error == 0)
				continue;
			else
				break;
		}

		/*
		 * Check permissions for special properties.
		 */
		switch (prop) {
		case ZFS_PROP_ZONED:
			/*
			 * Disallow setting of 'zoned' from within a local zone.
			 */
			if (!INGLOBALZONE(curproc))
				return (EPERM);
			break;

		case ZFS_PROP_QUOTA:
			if (error = zfs_dozonecheck(name, cr))
				return (error);

			if (!INGLOBALZONE(curproc)) {
				uint64_t zoned;
				char setpoint[MAXNAMELEN];
				int dslen;
				/*
				 * Unprivileged users are allowed to modify the
				 * quota on things *under* (ie. contained by)
				 * the thing they own.
				 */
				if (dsl_prop_get_integer(name, "zoned", &zoned,
				    setpoint))
					return (EPERM);
				if (!zoned) /* this shouldn't happen */
					return (EPERM);
				dslen = strlen(name);
				if (dslen <= strlen(setpoint))
					return (EPERM);
			}
			break;

		default:
			break;
		}

		switch (prop) {
		case ZFS_PROP_QUOTA:
			if ((error = nvpair_value_uint64(elem, &intval)) != 0 ||
			    (error = dsl_dir_set_quota(name,
			    intval)) != 0)
				return (error);
			break;

		case ZFS_PROP_RESERVATION:
			if ((error = nvpair_value_uint64(elem, &intval)) != 0 ||
			    (error = dsl_dir_set_reservation(name,
			    intval)) != 0)
				return (error);
			break;

		case ZFS_PROP_VOLSIZE:
			/* ZFSFUSE: ZVols not implemented */
			return ENXIO;
			break;

		case ZFS_PROP_VOLBLOCKSIZE:
			/* ZFSFUSE: ZVols not implemented */
			return ENXIO;
			break;

		default:
			if (nvpair_type(elem) == DATA_TYPE_STRING) {
				if (zfs_prop_get_type(prop) !=
				    prop_type_string)
					return (EINVAL);
				VERIFY(nvpair_value_string(elem, &strval) == 0);
				if ((error = dsl_prop_set(name,
				    nvpair_name(elem), 1, strlen(strval) + 1,
				    strval)) != 0)
					return (error);
			} else if (nvpair_type(elem) == DATA_TYPE_UINT64) {
				const char *unused;

				VERIFY(nvpair_value_uint64(elem, &intval) == 0);

				switch (zfs_prop_get_type(prop)) {
				case prop_type_number:
					break;
				case prop_type_boolean:
					if (intval > 1)
						return (EINVAL);
					break;
				case prop_type_string:
					return (EINVAL);
				case prop_type_index:
					if (zfs_prop_index_to_string(prop,
					    intval, &unused) != 0)
						return (EINVAL);
					break;
				default:
					cmn_err(CE_PANIC, "unknown property "
					    "type");
					break;
				}

				if ((error = dsl_prop_set(name, propname,
				    8, 1, &intval)) != 0)
					return (error);
			} else {
				return (EINVAL);
			}
			break;
		}
	}

	return (0);
}

static int
zfs_ioc_set_prop(zfs_cmd_t *zc)
{
	nvlist_t *nvl;
	int error;
	zfs_prop_t prop;

	/*
	 * If zc_value is set, then this is an attempt to inherit a value.
	 * Otherwise, zc_nvlist refers to a list of properties to set.
	 */
	if (zc->zc_value[0] != '\0') {
		if (!zfs_prop_user(zc->zc_value) &&
		    ((prop = zfs_name_to_prop(zc->zc_value)) ==
		    ZFS_PROP_INVAL ||
		    !zfs_prop_inheritable(prop)))
			return (EINVAL);

		return (dsl_prop_set(zc->zc_name, zc->zc_value, 0, 0, NULL));
	}

	if ((error = get_nvlist(zc, &nvl)) != 0)
		return (error);

	error = zfs_set_prop_nvlist(zc->zc_name, zc->zc_dev,
	    (cred_t *)(uintptr_t)zc->zc_cred, nvl);
	nvlist_free(nvl);
	return (error);
}

static int
zfs_ioc_create_minor(zfs_cmd_t *zc)
{
	/* ZFSFUSE TODO: implement ZVOLs */
	return ENXIO;
}

static int
zfs_ioc_remove_minor(zfs_cmd_t *zc)
{
	/* ZFSFUSE TODO: implement ZVOLs */
	return ENXIO;
}

/*
 * Search the vfs list for a specified resource.  Returns a pointer to it
 * or NULL if no suitable entry is found. The caller of this routine
 * is responsible for releasing the returned vfs pointer.
 */
#if 0
static vfs_t *
zfs_get_vfs(const char *resource)
{
	struct vfs *vfsp;
	struct vfs *vfs_found = NULL;

	vfs_list_read_lock();
	vfsp = rootvfs;
	do {
		if (strcmp(refstr_value(vfsp->vfs_resource), resource) == 0) {
			VFS_HOLD(vfsp);
			vfs_found = vfsp;
			break;
		}
		vfsp = vfsp->vfs_next;
	} while (vfsp != rootvfs);
	vfs_list_unlock();
	return (vfs_found);
}
#endif

static void
zfs_create_cb(objset_t *os, void *arg, dmu_tx_t *tx)
{
	/* ZFSFUSE: TODO */
}

static int
zfs_ioc_create(zfs_cmd_t *zc)
{
	objset_t *clone;
	int error = 0;
	zfs_create_data_t cbdata = { 0 };
	void (*cbfunc)(objset_t *os, void *arg, dmu_tx_t *tx);
	dmu_objset_type_t type = zc->zc_objset_type;

	switch (type) {

	case DMU_OST_ZFS:
		cbfunc = zfs_create_cb;
		break;
	case DMU_OST_ZVOL:
		/* ZFSFUSE: TODO Implement ZVOLs */
		/*cbfunc = zvol_create_cb;*/
		return ENOSYS;
	default:
		cbfunc = NULL;
	}
	if (strchr(zc->zc_name, '@'))
		return (EINVAL);

	if (zc->zc_nvlist_src != (uint64_t)(uintptr_t) NULL &&
	    (error = get_nvlist(zc, &cbdata.zc_props)) != 0)
		return (error);

	cbdata.zc_cred = (cred_t *)(uintptr_t)zc->zc_cred;
	cbdata.zc_dev = (dev_t)zc->zc_dev;

	if (zc->zc_value[0] != '\0') {
		/*
		 * We're creating a clone of an existing snapshot.
		 */
		zc->zc_value[sizeof (zc->zc_value) - 1] = '\0';
		if (dataset_namecheck(zc->zc_value, NULL, NULL) != 0) {
			nvlist_free(cbdata.zc_props);
			return (EINVAL);
		}

		error = dmu_objset_open(zc->zc_value, type,
		    DS_MODE_STANDARD | DS_MODE_READONLY, &clone);
		if (error) {
			nvlist_free(cbdata.zc_props);
			return (error);
		}
		error = dmu_objset_create(zc->zc_name, type, clone, NULL, NULL);
		dmu_objset_close(clone);
	} else {
		if (cbfunc == NULL) {
			nvlist_free(cbdata.zc_props);
			return (EINVAL);
		}

	/* ZFSFUSE: TODO Implement ZVOLs */
#if 0
		if (type == DMU_OST_ZVOL) {
			uint64_t volsize, volblocksize;

			if (cbdata.zc_props == NULL ||
			    nvlist_lookup_uint64(cbdata.zc_props,
			    zfs_prop_to_name(ZFS_PROP_VOLSIZE),
			    &volsize) != 0) {
				nvlist_free(cbdata.zc_props);
				return (EINVAL);
			}

			if ((error = nvlist_lookup_uint64(cbdata.zc_props,
			    zfs_prop_to_name(ZFS_PROP_VOLBLOCKSIZE),
			    &volblocksize)) != 0 && error != ENOENT) {
				nvlist_free(cbdata.zc_props);
				return (EINVAL);
			}

			if (error != 0)
				volblocksize = zfs_prop_default_numeric(
				    ZFS_PROP_VOLBLOCKSIZE);

			if ((error = zvol_check_volblocksize(
			    volblocksize)) != 0 ||
			    (error = zvol_check_volsize(volsize,
			    volblocksize)) != 0) {
				nvlist_free(cbdata.zc_props);
				return (error);
			}
		}
#endif

		error = dmu_objset_create(zc->zc_name, type, NULL, cbfunc,
		    &cbdata);
	}

	/*
	 * It would be nice to do this atomically.
	 */
	if (error == 0) {
		if ((error = zfs_set_prop_nvlist(zc->zc_name,
		    zc->zc_dev, (cred_t *)(uintptr_t)zc->zc_cred,
		    cbdata.zc_props)) != 0)
			(void) dmu_objset_destroy(zc->zc_name);
	}

	nvlist_free(cbdata.zc_props);
	return (error);
}

static int
zfs_ioc_snapshot(zfs_cmd_t *zc)
{
	if (snapshot_namecheck(zc->zc_value, NULL, NULL) != 0)
		return (EINVAL);
	return (dmu_objset_snapshot(zc->zc_name,
	    zc->zc_value, zc->zc_cookie));
}

static int
zfs_unmount_snap(char *name, void *arg)
{
	/* ZFSFUSE: TODO */
#if 0
	char *snapname = arg;
	char *cp;
	vfs_t *vfsp = NULL;

	/*
	 * Snapshots (which are under .zfs control) must be unmounted
	 * before they can be destroyed.
	 */

	if (snapname) {
		(void) strcat(name, "@");
		(void) strcat(name, snapname);
		vfsp = zfs_get_vfs(name);
		cp = strchr(name, '@');
		*cp = '\0';
	} else if (strchr(name, '@')) {
		vfsp = zfs_get_vfs(name);
	}

	if (vfsp) {
		/*
		 * Always force the unmount for snapshots.
		 */
		int flag = MS_FORCE;
		int err;

		if ((err = vn_vfswlock(vfsp->vfs_vnodecovered)) != 0) {
			VFS_RELE(vfsp);
			return (err);
		}
		VFS_RELE(vfsp);
		if ((err = dounmount(vfsp, flag, kcred)) != 0)
			return (err);
	}
#endif
	return (0);
}

static int
zfs_ioc_destroy_snaps(zfs_cmd_t *zc)
{
	int err;

	if (snapshot_namecheck(zc->zc_value, NULL, NULL) != 0)
		return (EINVAL);
	err = dmu_objset_find(zc->zc_name,
	    zfs_unmount_snap, zc->zc_value, DS_FIND_CHILDREN);
	if (err)
		return (err);
	return (dmu_snapshots_destroy(zc->zc_name, zc->zc_value));
}

static int
zfs_ioc_destroy(zfs_cmd_t *zc)
{
	if (strchr(zc->zc_name, '@') && zc->zc_objset_type == DMU_OST_ZFS) {
		int err = zfs_unmount_snap(zc->zc_name, NULL);
		if (err)
			return (err);
	}

	return (dmu_objset_destroy(zc->zc_name));
}

static int
zfs_ioc_rollback(zfs_cmd_t *zc)
{
	return (dmu_objset_rollback(zc->zc_name));
}

static int
zfs_ioc_rename(zfs_cmd_t *zc)
{
	zc->zc_value[sizeof (zc->zc_value) - 1] = '\0';
	if (dataset_namecheck(zc->zc_value, NULL, NULL) != 0)
		return (EINVAL);

	if (strchr(zc->zc_name, '@') != NULL &&
	    zc->zc_objset_type == DMU_OST_ZFS) {
		int err = zfs_unmount_snap(zc->zc_name, NULL);
		if (err)
			return (err);
	}

	return (dmu_objset_rename(zc->zc_name, zc->zc_value));
}

static int
zfs_ioc_recvbackup(zfs_cmd_t *zc)
{
/* zfs-fuse: TODO */
#if 0
	file_t *fp;
	int error, fd;
	offset_t new_off;

	if (dataset_namecheck(zc->zc_value, NULL, NULL) != 0 ||
	    strchr(zc->zc_value, '@') == NULL)
		return (EINVAL);

	fd = zc->zc_cookie;
	fp = getf(fd);
	if (fp == NULL)
		return (EBADF);
	error = dmu_recvbackup(zc->zc_value, &zc->zc_begin_record,
	    &zc->zc_cookie, (boolean_t)zc->zc_guid, fp->f_vnode,
	    fp->f_offset);

	new_off = fp->f_offset + zc->zc_cookie;
	if (VOP_SEEK(fp->f_vnode, fp->f_offset, &new_off) == 0)
		fp->f_offset = new_off;

	releasef(fd);
	return (error);
#endif
	return EBADF;
}

static int
zfs_ioc_sendbackup(zfs_cmd_t *zc)
{
/* zfs-fuse: TODO */
#if 0
	objset_t *fromsnap = NULL;
	objset_t *tosnap;
	file_t *fp;
	int error;

	error = dmu_objset_open(zc->zc_name, DMU_OST_ANY,
	    DS_MODE_STANDARD | DS_MODE_READONLY, &tosnap);
	if (error)
		return (error);

	if (zc->zc_value[0] != '\0') {
		char buf[MAXPATHLEN];
		char *cp;

		(void) strncpy(buf, zc->zc_name, sizeof (buf));
		cp = strchr(buf, '@');
		if (cp)
			*(cp+1) = 0;
		(void) strncat(buf, zc->zc_value, sizeof (buf));
		error = dmu_objset_open(buf, DMU_OST_ANY,
		    DS_MODE_STANDARD | DS_MODE_READONLY, &fromsnap);
		if (error) {
			dmu_objset_close(tosnap);
			return (error);
		}
	}

	fp = getf(zc->zc_cookie);
	if (fp == NULL) {
		dmu_objset_close(tosnap);
		if (fromsnap)
			dmu_objset_close(fromsnap);
		return (EBADF);
	}

	error = dmu_sendbackup(tosnap, fromsnap, fp->f_vnode);

	releasef(zc->zc_cookie);
	if (fromsnap)
		dmu_objset_close(fromsnap);
	dmu_objset_close(tosnap);
	return (error);
#endif
	return EBADF;
}

static int
zfs_ioc_inject_fault(zfs_cmd_t *zc)
{
	int id, error;

	error = zio_inject_fault(zc->zc_name, (int)zc->zc_guid, &id,
	    &zc->zc_inject_record);

	if (error == 0)
		zc->zc_guid = (uint64_t)id;

	return (error);
}

static int
zfs_ioc_clear_fault(zfs_cmd_t *zc)
{
	return (zio_clear_fault((int)zc->zc_guid));
}

static int
zfs_ioc_inject_list_next(zfs_cmd_t *zc)
{
	int id = (int)zc->zc_guid;
	int error;

	error = zio_inject_list_next(&id, zc->zc_name, sizeof (zc->zc_name),
	    &zc->zc_inject_record);

	zc->zc_guid = id;

	return (error);
}

static int
zfs_ioc_error_log(zfs_cmd_t *zc)
{
	spa_t *spa;
	int error;
	size_t count = (size_t)zc->zc_nvlist_dst_size;

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0)
		return (error);

	error = spa_get_errlog(spa, (void *)(uintptr_t)zc->zc_nvlist_dst,
	    &count);
	if (error == 0)
		zc->zc_nvlist_dst_size = count;
	else
		zc->zc_nvlist_dst_size = spa_get_errlog_size(spa);

	spa_close(spa, FTAG);

	return (error);
}

static int
zfs_ioc_clear(zfs_cmd_t *zc)
{
	spa_t *spa;
	vdev_t *vd;
	int error;

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0)
		return (error);

	spa_config_enter(spa, RW_WRITER, FTAG);

	if (zc->zc_guid == 0) {
		vd = NULL;
	} else if ((vd = spa_lookup_by_guid(spa, zc->zc_guid)) == NULL) {
		spa_config_exit(spa, FTAG);
		spa_close(spa, FTAG);
		return (ENODEV);
	}

	vdev_clear(spa, vd);

	spa_config_exit(spa, FTAG);

	spa_close(spa, FTAG);

	return (0);
}

static int
zfs_ioc_bookmark_name(zfs_cmd_t *zc)
{
	spa_t *spa;
	int error;
	nvlist_t *nvl;

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0)
		return (error);

	VERIFY(nvlist_alloc(&nvl, NV_UNIQUE_NAME, KM_SLEEP) == 0);

	error = spa_bookmark_name(spa, &zc->zc_bookmark, nvl);
	if (error == 0)
		error = put_nvlist(zc, nvl);
	nvlist_free(nvl);

	spa_close(spa, FTAG);

	return (error);
}

static int
zfs_ioc_promote(zfs_cmd_t *zc)
{
	char *cp;

	/*
	 * We don't need to unmount *all* the origin fs's snapshots, but
	 * it's easier.
	 */
	cp = strchr(zc->zc_value, '@');
	if (cp)
		*cp = '\0';
	(void) dmu_objset_find(zc->zc_value,
	    zfs_unmount_snap, NULL, DS_FIND_SNAPSHOTS);
	return (dsl_dataset_promote(zc->zc_name));
}

static zfs_ioc_vec_t zfs_ioc_vec[] = {
	{ zfs_ioc_pool_create,		zfs_secpolicy_config,	pool_name },
	{ zfs_ioc_pool_destroy,		zfs_secpolicy_config,	pool_name },
	{ zfs_ioc_pool_import,		zfs_secpolicy_config,	pool_name },
	{ zfs_ioc_pool_export,		zfs_secpolicy_config,	pool_name },
	{ zfs_ioc_pool_configs,		zfs_secpolicy_none,	no_name },
	{ zfs_ioc_pool_stats,		zfs_secpolicy_read,	pool_name },
	{ zfs_ioc_pool_tryimport,	zfs_secpolicy_config,	no_name },
	{ zfs_ioc_pool_scrub,		zfs_secpolicy_config,	pool_name },
	{ zfs_ioc_pool_freeze,		zfs_secpolicy_config,	no_name },
	{ zfs_ioc_pool_upgrade,		zfs_secpolicy_config,	pool_name },
	{ zfs_ioc_pool_get_history,	zfs_secpolicy_config,	pool_name },
	{ zfs_ioc_pool_log_history,	zfs_secpolicy_config,	pool_name },
	{ zfs_ioc_vdev_add,		zfs_secpolicy_config,	pool_name },
	{ zfs_ioc_vdev_remove,		zfs_secpolicy_config,	pool_name },
	{ zfs_ioc_vdev_online,		zfs_secpolicy_config,	pool_name },
	{ zfs_ioc_vdev_offline,		zfs_secpolicy_config,	pool_name },
	{ zfs_ioc_vdev_attach,		zfs_secpolicy_config,	pool_name },
	{ zfs_ioc_vdev_detach,		zfs_secpolicy_config,	pool_name },
	{ zfs_ioc_vdev_setpath,		zfs_secpolicy_config,	pool_name },
	{ zfs_ioc_objset_stats,		zfs_secpolicy_read,	dataset_name },
	{ zfs_ioc_dataset_list_next,	zfs_secpolicy_read,	dataset_name },
	{ zfs_ioc_snapshot_list_next,	zfs_secpolicy_read,	dataset_name },
	{ zfs_ioc_set_prop,		zfs_secpolicy_write,	dataset_name },
	{ zfs_ioc_create_minor,		zfs_secpolicy_config,	dataset_name },
	{ zfs_ioc_remove_minor,		zfs_secpolicy_config,	dataset_name },
	{ zfs_ioc_create,		zfs_secpolicy_parent,	dataset_name },
	{ zfs_ioc_destroy,		zfs_secpolicy_parent,	dataset_name },
	{ zfs_ioc_rollback,		zfs_secpolicy_write,	dataset_name },
	{ zfs_ioc_rename,		zfs_secpolicy_write,	dataset_name },
	{ zfs_ioc_recvbackup,		zfs_secpolicy_write,	dataset_name },
	{ zfs_ioc_sendbackup,		zfs_secpolicy_write,	dataset_name },
	{ zfs_ioc_inject_fault,		zfs_secpolicy_inject,	no_name },
	{ zfs_ioc_clear_fault,		zfs_secpolicy_inject,	no_name },
	{ zfs_ioc_inject_list_next,	zfs_secpolicy_inject,	no_name },
	{ zfs_ioc_error_log,		zfs_secpolicy_inject,	pool_name },
	{ zfs_ioc_clear,		zfs_secpolicy_config,	pool_name },
	{ zfs_ioc_bookmark_name,	zfs_secpolicy_inject,	pool_name },
	{ zfs_ioc_promote,		zfs_secpolicy_write,	dataset_name },
	{ zfs_ioc_destroy_snaps,	zfs_secpolicy_write,	dataset_name },
	{ zfs_ioc_snapshot,		zfs_secpolicy_write,	dataset_name }
};

int
zfsdev_ioctl(dev_t dev, int cmd, intptr_t arg, int flag, cred_t *cr, int *rvalp)
{
	zfs_cmd_t *zc;
	uint_t vec;
	int error, rc;

/* zfs-fuse: not implemented */
#if 0
	if (getminor(dev) != 0)
		return (zvol_ioctl(dev, cmd, arg, flag, cr, rvalp));
#endif

	vec = cmd - ZFS_IOC;

	if (vec >= sizeof (zfs_ioc_vec) / sizeof (zfs_ioc_vec[0]))
		return (EINVAL);

	zc = kmem_zalloc(sizeof (zfs_cmd_t), KM_SLEEP);

	error = xcopyin((void *)arg, zc, sizeof (zfs_cmd_t));

	if (error == 0) {
		zc->zc_cred = (uintptr_t)cr;
		zc->zc_dev = dev;
		error = zfs_ioc_vec[vec].zvec_secpolicy(zc->zc_name, cr);
	}

	/*
	 * Ensure that all pool/dataset names are valid before we pass down to
	 * the lower layers.
	 */
	if (error == 0) {
		zc->zc_name[sizeof (zc->zc_name) - 1] = '\0';
		switch (zfs_ioc_vec[vec].zvec_namecheck) {
		case pool_name:
			if (pool_namecheck(zc->zc_name, NULL, NULL) != 0)
				error = EINVAL;
			break;

		case dataset_name:
			if (dataset_namecheck(zc->zc_name, NULL, NULL) != 0)
				error = EINVAL;
			break;

		case no_name:
			break;
		}
	}

	if (error == 0)
		error = zfs_ioc_vec[vec].zvec_func(zc);

	rc = xcopyout(zc, (void *)arg, sizeof (zfs_cmd_t));
	if (error == 0)
		error = rc;

	kmem_free(zc, sizeof (zfs_cmd_t));
	return (error);
}

#if 0
static int
zfs_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	if (cmd != DDI_ATTACH)
		return (DDI_FAILURE);

	if (ddi_create_minor_node(dip, "zfs", S_IFCHR, 0,
	    DDI_PSEUDO, 0) == DDI_FAILURE)
		return (DDI_FAILURE);

	zfs_dip = dip;

	ddi_report_dev(dip);

	return (DDI_SUCCESS);
}

static int
zfs_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	if (spa_busy() || zfs_busy() || zvol_busy())
		return (DDI_FAILURE);

	if (cmd != DDI_DETACH)
		return (DDI_FAILURE);

	zfs_dip = NULL;

	ddi_prop_remove_all(dip);
	ddi_remove_minor_node(dip, NULL);

	return (DDI_SUCCESS);
}

/*ARGSUSED*/
static int
zfs_info(dev_info_t *dip, ddi_info_cmd_t infocmd, void *arg, void **result)
{
	switch (infocmd) {
	case DDI_INFO_DEVT2DEVINFO:
		*result = zfs_dip;
		return (DDI_SUCCESS);

	case DDI_INFO_DEVT2INSTANCE:
		*result = (void *)0;
		return (DDI_SUCCESS);
	}

	return (DDI_FAILURE);
}

/*
 * OK, so this is a little weird.
 *
 * /dev/zfs is the control node, i.e. minor 0.
 * /dev/zvol/[r]dsk/pool/dataset are the zvols, minor > 0.
 *
 * /dev/zfs has basically nothing to do except serve up ioctls,
 * so most of the standard driver entry points are in zvol.c.
 */
static struct cb_ops zfs_cb_ops = {
	zvol_open,	/* open */
	zvol_close,	/* close */
	zvol_strategy,	/* strategy */
	nodev,		/* print */
	nodev,		/* dump */
	zvol_read,	/* read */
	zvol_write,	/* write */
	zfsdev_ioctl,	/* ioctl */
	nodev,		/* devmap */
	nodev,		/* mmap */
	nodev,		/* segmap */
	nochpoll,	/* poll */
	ddi_prop_op,	/* prop_op */
	NULL,		/* streamtab */
	D_NEW | D_MP | D_64BIT,		/* Driver compatibility flag */
	CB_REV,		/* version */
	zvol_aread,	/* async read */
	zvol_awrite,	/* async write */
};

static struct dev_ops zfs_dev_ops = {
	DEVO_REV,	/* version */
	0,		/* refcnt */
	zfs_info,	/* info */
	nulldev,	/* identify */
	nulldev,	/* probe */
	zfs_attach,	/* attach */
	zfs_detach,	/* detach */
	nodev,		/* reset */
	&zfs_cb_ops,	/* driver operations */
	NULL		/* no bus operations */
};

static struct modldrv zfs_modldrv = {
	&mod_driverops, "ZFS storage pool version " ZFS_VERSION_STRING,
	    &zfs_dev_ops
};

static struct modlinkage modlinkage = {
	MODREV_1,
	(void *)&zfs_modlfs,
	(void *)&zfs_modldrv,
	NULL
};
#endif

int
zfs_ioctl_init(void)
{
	spa_init(FREAD | FWRITE);

	zfs_init();

	/* zfs-fuse: not implemented */
	/*zvol_init();*/

#if 0
	if ((error = mod_install(&modlinkage)) != 0) {
		zvol_fini();
		zfs_fini();
		spa_fini();
		return (error);
	}
	error = ldi_ident_from_mod(&modlinkage, &zfs_li);
	ASSERT(error == 0);
#endif

	return (0);
}

int
zfs_ioctl_fini(void)
{
	int error = 0;

	if (spa_busy() || zfs_busy() || /*zvol_busy() ||*/ zio_injection_enabled)
		return (EBUSY);

#if 0
	if ((error = mod_remove(&modlinkage)) != 0)
		return (error);
#endif

	/* zfs-fuse: not implemented */
	/*zvol_fini();*/

	zfs_fini();
	spa_fini();

#if 0
	ldi_ident_release(zfs_li);
	zfs_li = NULL;
#endif

	return (error);
}

#if 0
int
_info(struct modinfo *modinfop)
{
	return (mod_info(&modlinkage, modinfop));
}
#endif
