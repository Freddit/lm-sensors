/*
    sysfs.c - Part of libsensors, a library for reading Linux sensor data
    Copyright (c) 2005 Mark M. Hoffman <mhoffman@lightlink.com>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/* this define needed for strndup() */
#define _GNU_SOURCE

#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include <sysfs/libsysfs.h>
#include "data.h"
#include "error.h"
#include "access.h"
#include "general.h"
#include "sysfs.h"

int sensors_found_sysfs = 0;

char sensors_sysfs_mount[NAME_MAX];

#define MAX_SENSORS_PER_TYPE 16

static
int get_type_scaling(int type)
{
	switch (type & 0xFF10) {
	case SENSORS_FEATURE_IN:
	case SENSORS_FEATURE_TEMP:
		return 3;
	case SENSORS_FEATURE_FAN:
		return 0;
	}

	switch (type) {
	case SENSORS_FEATURE_VID:
		return 3;
	case SENSORS_FEATURE_VRM:
		return 1;
	default:
		return 0;
	}

	return 0;
}

static 
sensors_chip_features sensors_read_dynamic_chip(struct sysfs_device *sysdir)
{
	int i, type, fnum = 1;
	struct sysfs_attribute *attr;
	struct dlist *attrs;
	sensors_chip_features ret = {0, 0};
	/* room for all 3  (in, fan, temp) types, with all their subfeatures
	   + misc features. We use a large sparse table at first to store all
	   found features, so that we can store them sorted at type and index
	   and then later create a dense sorted table */
	sensors_chip_feature features[MAX_SENSORS_PER_TYPE *
		SENSORS_FEATURE_MAX_SUB_FEATURES * 3 +
		SENSORS_FEATURE_MAX_SUB_FEATURES];
	sensors_chip_feature *dyn_features;
	char *name;
		
	attrs = sysfs_get_device_attributes(sysdir);
	
	if (attrs == NULL)
		return ret;
		
	memset(features, 0, sizeof(features));
	
	dlist_for_each_data(attrs, attr, struct sysfs_attribute) {
		sensors_chip_feature feature = { { 0, }, 0, };
		name = attr->name;
		
		if (!strcmp(name, "name")) {
			ret.prefix = strndup(attr->value, strlen(attr->value) - 1);
			continue;
		} 
		
		/* check for _input extension and remove */
		i = strlen(name);
		if (i > 6 && !strcmp(name + i - 6, "_input"))
			feature.data.name = strndup(name, i-6);
		else
			feature.data.name = strdup(name);

		type = sensors_feature_get_type(&feature.data);
		if (type == SENSORS_FEATURE_UNKNOWN) {
			free((char *)feature.data.name);
			continue;
		}
			
		/* Get N as in this is the N-th in / fan / temp sensor */
		switch (type & 0xFF00) {
			case SENSORS_FEATURE_IN:
				i = strtol(name + 2, NULL, 10);
				break;
			case SENSORS_FEATURE_FAN:
				i = strtol(name + 3, NULL, 10);
				if (i) i--;
				break;
			case SENSORS_FEATURE_TEMP:
				i = strtol(name + 4, NULL, 10);
				if (i) i--;
				break;
			case SENSORS_FEATURE_VID: /* first misc feature */
				i = 0;
				break;
		}
		
		if (i >= MAX_SENSORS_PER_TYPE) {
			fprintf(stderr, "libsensors error, more sensors of one"
				" type then MAX_SENSORS_PER_TYPE, ignoring "
				"feature: %s\n", name);
			free((char *)feature.data.name);
			continue;
		}
		
		/* "calculate" a place to store the feature in our sparse,
		   sorted table */
		i = (type >> 8) * MAX_SENSORS_PER_TYPE *
			SENSORS_FEATURE_MAX_SUB_FEATURES +
			i * SENSORS_FEATURE_MAX_SUB_FEATURES + (type & 0xFF);
		
		if (features[i].data.name) {			
			fprintf(stderr, "libsensors error, trying to add dupli"
				"cate feature: %s to dynamic feature table\n",
				name);
			free((char *)feature.data.name);
			continue;
		}
		
		/* fill in the other feature members */
		feature.data.number = i + 1;
			
		if ( (type & 0xFF00) == SENSORS_FEATURE_VID ||
				(type & 0x00FF) == 0) {
			/* misc sensor or main feature */
			feature.data.mapping = SENSORS_NO_MAPPING;
			feature.data.compute_mapping = SENSORS_NO_MAPPING;
		} else if (type & 0x10) {
			/* sub feature without compute mapping */
			feature.data.mapping = i -
				i % SENSORS_FEATURE_MAX_SUB_FEATURES + 1;
			feature.data.compute_mapping = SENSORS_NO_MAPPING;
		} else {
			feature.data.mapping = i -
				i % SENSORS_FEATURE_MAX_SUB_FEATURES + 1;
			feature.data.compute_mapping = feature.data.mapping;
		}
		
		feature.data.mode =
			(attr->method & (SYSFS_METHOD_SHOW|SYSFS_METHOD_STORE))
			 == (SYSFS_METHOD_SHOW|SYSFS_METHOD_STORE) ?
			SENSORS_MODE_RW : (attr->method & SYSFS_METHOD_SHOW) ?
			SENSORS_MODE_R : (attr->method & SYSFS_METHOD_STORE) ?
			SENSORS_MODE_W : SENSORS_MODE_NO_RW;

		feature.scaling = get_type_scaling(type);

		features[i] = feature;
		fnum++;
	}

	dyn_features = calloc(fnum, sizeof(sensors_chip_feature));
	if (dyn_features == NULL) {
		sensors_fatal_error(__FUNCTION__,"Out of memory");
	}
	
	fnum = 0;
	for(i = 0; i < sizeof(features)/sizeof(sensors_chip_feature); i++) {
		if (features[i].data.name) {
			dyn_features[fnum] = features[i];
			fnum++;
		}
	}
	
	ret.feature = dyn_features;
	
	return ret;
}

/* returns !0 if sysfs filesystem was found, 0 otherwise */
int sensors_init_sysfs(void)
{
	if (sysfs_get_mnt_path(sensors_sysfs_mount, NAME_MAX) == 0)
		sensors_found_sysfs = 1;

	return sensors_found_sysfs;
}

/* returns: 0 if successful, !0 otherwise */
static int sensors_read_one_sysfs_chip(struct sysfs_device *dev)
{
	static int total_dynamic = 0;
	int domain, bus, slot, fn, i;
	struct sysfs_attribute *attr, *bus_attr;
	char bus_path[SYSFS_PATH_MAX];
	sensors_proc_chips_entry entry;

	/* ignore any device without name attribute */
	if (!(attr = sysfs_get_device_attr(dev, "name")))
		return 0;

	/* ignore subclients */
	if (attr->len >= 11 && !strcmp(attr->value + attr->len - 11,
			" subclient\n"))
		return 0;

	/* NB: attr->value[attr->len-1] == '\n'; chop that off */
	entry.name.prefix = strndup(attr->value, attr->len - 1);
	if (!entry.name.prefix)
		sensors_fatal_error(__FUNCTION__, "out of memory");

	entry.name.busname = strdup(dev->path);
	if (!entry.name.busname)
		sensors_fatal_error(__FUNCTION__, "out of memory");

	if (sscanf(dev->name, "%d-%x", &entry.name.bus, &entry.name.addr) == 2) {
		/* find out if legacy ISA or not */
		if (entry.name.bus == 9191)
			entry.name.bus = SENSORS_CHIP_NAME_BUS_ISA;
		else {
			snprintf(bus_path, sizeof(bus_path),
				"%s/class/i2c-adapter/i2c-%d/device/name",
				sensors_sysfs_mount, entry.name.bus);

			if ((bus_attr = sysfs_open_attribute(bus_path))) {
				if (sysfs_read_attribute(bus_attr))
					return -SENSORS_ERR_PARSE;

				if (bus_attr->value
				 && !strncmp(bus_attr->value, "ISA ", 4))
					entry.name.bus = SENSORS_CHIP_NAME_BUS_ISA;

				sysfs_close_attribute(bus_attr);
			}
		}
	} else if (sscanf(dev->name, "%*[a-z0-9_].%d", &entry.name.addr) == 1) {
		/* must be new ISA (platform driver) */
		entry.name.bus = SENSORS_CHIP_NAME_BUS_ISA;
	} else if (sscanf(dev->name, "%x:%x:%x.%x", &domain, &bus, &slot, &fn) == 4) {
		/* PCI */
		entry.name.addr = (domain << 16) + (bus << 8) + (slot << 3) + fn;
		entry.name.bus = SENSORS_CHIP_NAME_BUS_PCI;
	} else
		return -SENSORS_ERR_PARSE;
	
	/* check whether this chip is known in the static list */ 
	for (i = 0; sensors_chip_features_list[i].prefix; i++)
		if (!strcasecmp(sensors_chip_features_list[i].prefix, entry.name.prefix))
			break;

	/* if no chip definition matches */
	if (!sensors_chip_features_list[i].prefix && 
		total_dynamic < N_PLACEHOLDER_ELEMENTS) {
		sensors_chip_features n_entry = sensors_read_dynamic_chip(dev);

		/* skip to end of list */
		for(i = 0; sensors_chip_features_list[i].prefix; i++);

		sensors_chip_features_list[i] = n_entry;	

		total_dynamic++;
	}
		
	sensors_add_proc_chips(&entry);

	return 0;
}

/* returns 0 if successful, !0 otherwise */
static int sensors_read_sysfs_chips_compat(void)
{
	struct sysfs_bus *bus;
	struct dlist *devs;
	struct sysfs_device *dev;
	int ret = 0;

	if (!(bus = sysfs_open_bus("i2c"))) {
		if (errno && errno != ENOENT)
			ret = -SENSORS_ERR_PROC;
		goto exit0;
	}

	if (!(devs = sysfs_get_bus_devices(bus))) {
		if (errno && errno != ENOENT)
			ret = -SENSORS_ERR_PROC;
		goto exit1;
	}

	dlist_for_each_data(devs, dev, struct sysfs_device)
		if ((ret = sensors_read_one_sysfs_chip(dev)))
			goto exit1;

exit1:
	/* this frees bus and devs */
	sysfs_close_bus(bus);

exit0:
	return ret;
}

/* returns 0 if successful, !0 otherwise */
int sensors_read_sysfs_chips(void)
{
	struct sysfs_class *cls;
	struct dlist *clsdevs;
	struct sysfs_class_device *clsdev;
	int ret = 0;

	if (!(cls = sysfs_open_class("hwmon"))) {
		/* compatibility function for kernel 2.6.n where n <= 13 */
		return sensors_read_sysfs_chips_compat();
	}

	if (!(clsdevs = sysfs_get_class_devices(cls))) {
		if (errno && errno != ENOENT)
			ret = -SENSORS_ERR_PROC;
		goto exit;
	}

	dlist_for_each_data(clsdevs, clsdev, struct sysfs_class_device) {
		struct sysfs_device *dev;
		if (!(dev = sysfs_get_classdev_device(clsdev))) {
			ret = -SENSORS_ERR_PROC;
			goto exit;
		}
		if ((ret = sensors_read_one_sysfs_chip(dev)))
			goto exit;
	}

exit:
	/* this frees cls and clsdevs */
	sysfs_close_class(cls);
	return ret;
}

/* returns 0 if successful, !0 otherwise */
int sensors_read_sysfs_bus(void)
{
	struct sysfs_class *cls;
	struct dlist *clsdevs;
	struct sysfs_class_device *clsdev;
	sensors_bus entry;
	int ret = 0;

	if (!(cls = sysfs_open_class("i2c-adapter"))) {
		if (errno && errno != ENOENT)
			ret = -SENSORS_ERR_PROC;
		goto exit0;
	}

	if (!(clsdevs = sysfs_get_class_devices(cls))) {
		if (errno && errno != ENOENT)
			ret = -SENSORS_ERR_PROC;
		goto exit1;
	}

	dlist_for_each_data(clsdevs, clsdev, struct sysfs_class_device) {
		struct sysfs_device *dev;
		struct sysfs_attribute *attr;

		/* Get the adapter name from the classdev "name" attribute
		 * (Linux 2.6.20 and later). If it fails, fall back to
		 * the device "name" attribute (for older kernels). */
		if (!(attr = sysfs_get_classdev_attr(clsdev, "name"))
		 && !((dev = sysfs_get_classdev_device(clsdev)) &&
		      (attr = sysfs_get_device_attr(dev, "name"))))
			continue;

		/* NB: attr->value[attr->len-1] == '\n'; chop that off */
		entry.adapter = strndup(attr->value, attr->len - 1);
		if (!entry.adapter)
			sensors_fatal_error(__FUNCTION__, "out of memory");

		if (!strncmp(entry.adapter, "ISA ", 4)) {
			entry.number = SENSORS_CHIP_NAME_BUS_ISA;
		} else if (sscanf(clsdev->name, "i2c-%d", &entry.number) != 1) {
			entry.number = SENSORS_CHIP_NAME_BUS_DUMMY;
		}

		sensors_add_proc_bus(&entry);
	}

exit1:
	/* this frees *cls _and_ *clsdevs */
	sysfs_close_class(cls);

exit0:
	return ret;
}

