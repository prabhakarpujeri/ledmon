/*
 * Intel(R) Enclosure LED Utilities
 * Copyright (c) 2016-2022, Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "list.h"
#include "pci_slot.h"
#include "status.h"
#include "sysfs.h"
#include "utils.h"
#include "vmdssd.h"

#define ATTENTION_OFF        0xF  /* (1111) Attention Off, Power Off */
#define ATTENTION_LOCATE     0x7  /* (0111) Attention Off, Power On */
#define ATTENTION_REBUILD    0x5  /* (0101) Attention On, Power On */
#define ATTENTION_FAILURE    0xD  /* (1101) Attention On, Power Off */

struct ibpi_value ibpi_to_attention[] = {
	{IBPI_PATTERN_LOCATE, ATTENTION_LOCATE},
	{IBPI_PATTERN_FAILED_DRIVE, ATTENTION_FAILURE},
	{IBPI_PATTERN_REBUILD, ATTENTION_REBUILD},
	{IBPI_PATTERN_LOCATE_OFF, ATTENTION_OFF}
};

#define SYSFS_PCIEHP         "/sys/module/pciehp"

static char *get_slot_from_syspath(char *path)
{
	char *cur, *ret = NULL;
	char *temp_path = str_dup(path);

	cur = strtok(temp_path, "/");
	while (cur != NULL) {
		char *next = strtok(NULL, "/");

		if ((next != NULL) && strcmp(next, "nvme") == 0)
			break;
		cur = next;
	}

	cur = strtok(cur, ".");
	if (cur)
		ret = str_dup(cur);
	free(temp_path);

	return ret;
}

static int check_slot_module(const char *slot_path)
{
	char module_path[PATH_MAX], real_module_path[PATH_MAX];
	struct list dir;

	// check if slot is managed by pciehp driver
	snprintf(module_path, PATH_MAX, "%s/module", slot_path);
	if (scan_dir(module_path, &dir) == 0) {
		list_erase(&dir);
		if (realpath(module_path, real_module_path) == NULL)
			return -1;
		if (strcmp(real_module_path, SYSFS_PCIEHP) != 0)
			__set_errno_and_return(EINVAL);
	} else {
		__set_errno_and_return(ENOENT);
	}

	return 0;
}

struct pci_slot *vmdssd_find_pci_slot(char *device_path)
{
	char *pci_addr;
	struct pci_slot *slot = NULL;

	pci_addr = get_slot_from_syspath(device_path);
	if (!pci_addr)
		return NULL;

	list_for_each(sysfs_get_pci_slots(), slot) {
		if (strcmp(slot->address, pci_addr) == 0)
			break;
		slot = NULL;
	}
	free(pci_addr);
	if (slot == NULL || check_slot_module(slot->sysfs_path) < 0)
		return NULL;

	return slot;
}

status_t vmdssd_write_attention_buf(struct pci_slot *slot, enum ibpi_pattern ibpi)
{
	char attention_path[PATH_MAX];
	char buf[WRITE_BUFFER_SIZE];
	uint16_t val;

	log_debug("%s before: 0x%x\n", slot->address,
		  get_int(slot->sysfs_path, 0, "attention"));
	val = get_value_for_ibpi(ibpi, ibpi_to_attention);
	snprintf(buf, WRITE_BUFFER_SIZE, "%u", val);
	snprintf(attention_path, PATH_MAX, "%s/attention", slot->sysfs_path);
	if (buf_write(attention_path, buf) != (ssize_t) strnlen(buf, WRITE_BUFFER_SIZE)) {
		log_error("%s write error: %d\n", slot->sysfs_path, errno);
		return STATUS_FILE_WRITE_ERROR;
	}
	log_debug("%s after: 0x%x\n", slot->address,
		  get_int(slot->sysfs_path, 0, "attention"));

	return STATUS_SUCCESS;
}

int vmdssd_write(struct block_device *device, enum ibpi_pattern ibpi)
{
	struct pci_slot *slot;
	char *short_name = strrchr(device->sysfs_path, '/');

	if (short_name)
		short_name++;
	else
		short_name = device->sysfs_path;

	if (ibpi == device->ibpi_prev)
		return 0;

	if ((ibpi < IBPI_PATTERN_NORMAL) || (ibpi > IBPI_PATTERN_LOCATE_OFF))
		__set_errno_and_return(ERANGE);

	slot = vmdssd_find_pci_slot(device->sysfs_path);
	if (!slot) {
		log_debug("PCI hotplug slot not found for %s\n", short_name);
		__set_errno_and_return(ENODEV);
	}

	return vmdssd_write_attention_buf(slot, ibpi);
}

char *vmdssd_get_path(const char *cntrl_path)
{
	return str_dup(cntrl_path);
}
