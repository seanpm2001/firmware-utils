// SPDX-License-Identifier: BSD-2-Clause
/*
  Copyright (c) 2014, Matthias Schiffer <mschiffer@universe-factory.net>
  All rights reserved.
*/


/*
   tplink-safeloader

   Image generation tool for the TP-LINK SafeLoader as seen on
   TP-LINK Pharos devices (CPE210/220/510/520)
*/


#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>

#include "md5.h"


#define ALIGN(x,a) ({ typeof(a) __a = (a); (((x) + __a - 1) & ~(__a - 1)); })


#define MAX_PARTITIONS	32

/** An image partition table entry */
struct image_partition_entry {
	const char *name;
	size_t size;
	uint8_t *data;
};

/** A flash partition table entry */
struct flash_partition_entry {
	const char *name;
	uint32_t base;
	uint32_t size;
};

/** Flash partition names table entry */
struct factory_partition_names {
	const char *partition_table;
	const char *soft_ver;
	const char *os_image;
	const char *support_list;
	const char *file_system;
	const char *extra_para;
};

/** Partition trailing padding definitions
 * Values 0x00 to 0xff are reserved to indicate the padding value
 * Values from 0x100 are reserved to indicate other behaviour */
enum partition_trail_value {
	PART_TRAIL_00   = 0x00,
	PART_TRAIL_FF   = 0xff,
	PART_TRAIL_MAX  = 0xff,
	PART_TRAIL_NONE = 0x100
};

/** soft-version value overwrite types
 * The default (for an uninitialised soft_ver field) is to use the numerical
 * version number "0.0.0"
 */
enum soft_ver_type {
	SOFT_VER_TYPE_NUMERIC = 0,
	SOFT_VER_TYPE_TEXT = 1,
};

/** Firmware layout description */
struct device_info {
	const char *id;
	const char *vendor;
	const char *support_list;
	enum partition_trail_value part_trail;
	struct {
		enum soft_ver_type type;
		union {
			const char *text;
			uint8_t num[3];
		};
	} soft_ver;
	uint32_t soft_ver_compat_level;
	struct flash_partition_entry partitions[MAX_PARTITIONS+1];
	const char *first_sysupgrade_partition;
	const char *last_sysupgrade_partition;
	struct factory_partition_names partition_names;
};

#define SOFT_VER_TEXT(_t) {.type = SOFT_VER_TYPE_TEXT, .text = _t}
#define SOFT_VER_NUMERIC(_maj, _min, _patch) {  \
		.type = SOFT_VER_TYPE_NUMERIC,          \
		.num = {_maj, _min, _patch}}
#define SOFT_VER_DEFAULT SOFT_VER_NUMERIC(0, 0, 0)

struct __attribute__((__packed__)) meta_header {
	uint32_t length;
	uint32_t zero;
};

/** The content of the soft-version structure */
struct __attribute__((__packed__)) soft_version {
	uint8_t pad1;
	uint8_t version_major;
	uint8_t version_minor;
	uint8_t version_patch;
	uint8_t year_hi;
	uint8_t year_lo;
	uint8_t month;
	uint8_t day;
	uint32_t rev;
	uint32_t compat_level;
};

/*
 * Safeloader image type
 *   Safeloader images contain a 0x14 byte preamble with image size (big endian
 *   UINT32) and md5 checksum (16 bytes).
 *
 * SAFEFLOADER_TYPE_DEFAULT
 *   Standard preamble with size including preamble length, and checksum.
 *   Header of 0x1000 bytes, contents of which are not specified.
 *   Payload starts at offset 0x1014.
 *
 * SAFELOADER_TYPE_VENDOR
 *   Standard preamble with size including preamble length, and checksum.
 *   Header contains up to 0x1000 bytes of vendor data, starting with a big endian
 *   UINT32 size, followed by that number of bytes containing (text) data.
 *   Padded with 0xFF. Payload starts at offset 0x1014.
 *
 * SAFELOADER_TYPE_CLOUD
 *   Standard preamble with size including preamble length, and checksum.
 *   Followed by the 'fw-type:Cloud' string and some (unknown) data.
 *   Payload starts at offset 0x1014.
 *
 * SAFELOADER_TYPE_QNEW
 *   Reversed order preamble, with (apparent) md5 checksum before the image
 *   size. The size does not include the preamble length.
 *   Header starts with 0x3C bytes, starting with the string '?NEW' (format not
 *   understood). Then another 0x1000 bytes follow, with the data payload
 *   starting at 0x1050.
 */
enum safeloader_image_type {
	SAFELOADER_TYPE_DEFAULT,
	SAFELOADER_TYPE_VENDOR,
	SAFELOADER_TYPE_CLOUD,
	SAFELOADER_TYPE_QNEW,
};

/* Internal representation of safeloader image data */
struct safeloader_image_info {
	enum safeloader_image_type type;
	size_t payload_offset;
	struct flash_partition_entry entries[MAX_PARTITIONS];
};

#define SAFELOADER_PREAMBLE_SIZE	0x14
#define SAFELOADER_HEADER_SIZE		0x1000
#define SAFELOADER_PAYLOAD_OFFSET	(SAFELOADER_PREAMBLE_SIZE + SAFELOADER_HEADER_SIZE)

#define SAFELOADER_QNEW_HEADER_SIZE	0x3C
#define SAFELOADER_QNEW_PAYLOAD_OFFSET	\
	(SAFELOADER_PREAMBLE_SIZE + SAFELOADER_QNEW_HEADER_SIZE + SAFELOADER_HEADER_SIZE)

#define SAFELOADER_PAYLOAD_TABLE_SIZE	0x800

static const uint8_t jffs2_eof_mark[4] = {0xde, 0xad, 0xc0, 0xde};


/**
   Salt for the MD5 hash

   Fortunately, TP-LINK seems to use the same salt for most devices which use
   the new image format.
*/
static const uint8_t md5_salt[16] = {
	0x7a, 0x2b, 0x15, 0xed,
	0x9b, 0x98, 0x59, 0x6d,
	0xe5, 0x04, 0xab, 0x44,
	0xac, 0x2a, 0x9f, 0x4e,
};


/** Firmware layout table */
static struct device_info boards[] = {
	/** Firmware layout for the CPE210/220 V1 */
	{
		.id     = "CPE210",
		.vendor = "CPE510(TP-LINK|UN|N300-5):1.0\r\n",
		.support_list =
			"SupportList:\r\n"
			"CPE210(TP-LINK|UN|N300-2):1.0\r\n"
			"CPE210(TP-LINK|UN|N300-2):1.1\r\n"
			"CPE210(TP-LINK|US|N300-2):1.1\r\n"
			"CPE210(TP-LINK|EU|N300-2):1.1\r\n"
			"CPE220(TP-LINK|UN|N300-2):1.1\r\n"
			"CPE220(TP-LINK|US|N300-2):1.1\r\n"
			"CPE220(TP-LINK|EU|N300-2):1.1\r\n",
		.part_trail = 0xff,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"partition-table", 0x20000, 0x02000},
			{"default-mac", 0x30000, 0x00020},
			{"product-info", 0x31100, 0x00100},
			{"signature", 0x32000, 0x00400},
			{"firmware", 0x40000, 0x770000},
			{"soft-version", 0x7b0000, 0x00100},
			{"support-list", 0x7b1000, 0x00400},
			{"user-config", 0x7c0000, 0x10000},
			{"default-config", 0x7d0000, 0x10000},
			{"log", 0x7e0000, 0x10000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "support-list",
	},

	/** Firmware layout for the CPE210 V2 */
	{
		.id     = "CPE210V2",
		.vendor = "CPE210(TP-LINK|UN|N300-2|00000000):2.0\r\n",
		.support_list =
			"SupportList:\r\n"
			"CPE210(TP-LINK|EU|N300-2|00000000):2.0\r\n"
			"CPE210(TP-LINK|EU|N300-2|45550000):2.0\r\n"
			"CPE210(TP-LINK|EU|N300-2|55530000):2.0\r\n"
			"CPE210(TP-LINK|UN|N300-2|00000000):2.0\r\n"
			"CPE210(TP-LINK|UN|N300-2|45550000):2.0\r\n"
			"CPE210(TP-LINK|UN|N300-2|55530000):2.0\r\n"
			"CPE210(TP-LINK|US|N300-2|55530000):2.0\r\n"
			"CPE210(TP-LINK|UN|N300-2):2.0\r\n"
			"CPE210(TP-LINK|EU|N300-2):2.0\r\n"
			"CPE210(TP-LINK|US|N300-2):2.0\r\n",
		.part_trail = 0xff,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"partition-table", 0x20000, 0x02000},
			{"default-mac", 0x30000, 0x00020},
			{"product-info", 0x31100, 0x00100},
			{"device-info", 0x31400, 0x00400},
			{"signature", 0x32000, 0x00400},
			{"device-id", 0x33000, 0x00100},
			{"firmware", 0x40000, 0x770000},
			{"soft-version", 0x7b0000, 0x00100},
			{"support-list", 0x7b1000, 0x01000},
			{"user-config", 0x7c0000, 0x10000},
			{"default-config", 0x7d0000, 0x10000},
			{"log", 0x7e0000, 0x10000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "support-list",
	},

	/** Firmware layout for the CPE210 V3 */
	{
		.id     = "CPE210V3",
		.vendor = "CPE210(TP-LINK|UN|N300-2|00000000):3.0\r\n",
		.support_list =
			"SupportList:\r\n"
			"CPE210(TP-LINK|EU|N300-2|45550000):3.0\r\n"
			"CPE210(TP-LINK|UN|N300-2|00000000):3.0\r\n"
			"CPE210(TP-LINK|US|N300-2|55530000):3.0\r\n"
			"CPE210(TP-LINK|UN|N300-2):3.0\r\n"
			"CPE210(TP-LINK|EU|N300-2):3.0\r\n"
			"CPE210(TP-LINK|EU|N300-2|45550000):3.1\r\n"
			"CPE210(TP-LINK|UN|N300-2|00000000):3.1\r\n"
			"CPE210(TP-LINK|US|N300-2|55530000):3.1\r\n"
			"CPE210(TP-LINK|EU|N300-2|45550000):3.20\r\n"
			"CPE210(TP-LINK|UN|N300-2|00000000):3.20\r\n"
			"CPE210(TP-LINK|US|N300-2|55530000):3.20\r\n",
		.part_trail = 0xff,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"partition-table", 0x20000, 0x01000},
			{"default-mac", 0x30000, 0x00020},
			{"product-info", 0x31100, 0x00100},
			{"device-info", 0x31400, 0x00400},
			{"signature", 0x32000, 0x00400},
			{"device-id", 0x33000, 0x00100},
			{"firmware", 0x40000, 0x770000},
			{"soft-version", 0x7b0000, 0x00100},
			{"support-list", 0x7b1000, 0x01000},
			{"user-config", 0x7c0000, 0x10000},
			{"default-config", 0x7d0000, 0x10000},
			{"log", 0x7e0000, 0x10000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "support-list",
	},

	/** Firmware layout for the CPE220 V2 */
	{
		.id     = "CPE220V2",
		.vendor = "CPE510(TP-LINK|UN|N300-5):1.0\r\n",
		.support_list =
			"SupportList:\r\n"
			"CPE220(TP-LINK|EU|N300-2|00000000):2.0\r\n"
			"CPE220(TP-LINK|EU|N300-2|45550000):2.0\r\n"
			"CPE220(TP-LINK|EU|N300-2|55530000):2.0\r\n"
			"CPE220(TP-LINK|UN|N300-2|00000000):2.0\r\n"
			"CPE220(TP-LINK|UN|N300-2|45550000):2.0\r\n"
			"CPE220(TP-LINK|UN|N300-2|55530000):2.0\r\n"
			"CPE220(TP-LINK|US|N300-2|55530000):2.0\r\n"
			"CPE220(TP-LINK|UN|N300-2):2.0\r\n"
			"CPE220(TP-LINK|EU|N300-2):2.0\r\n"
			"CPE220(TP-LINK|US|N300-2):2.0\r\n",
		.part_trail = 0xff,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"partition-table", 0x20000, 0x02000},
			{"default-mac", 0x30000, 0x00020},
			{"product-info", 0x31100, 0x00100},
			{"signature", 0x32000, 0x00400},
			{"firmware", 0x40000, 0x770000},
			{"soft-version", 0x7b0000, 0x00100},
			{"support-list", 0x7b1000, 0x00400},
			{"user-config", 0x7c0000, 0x10000},
			{"default-config", 0x7d0000, 0x10000},
			{"log", 0x7e0000, 0x10000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "support-list",
	},

	/** Firmware layout for the CPE220 V3 */
	{
		.id     = "CPE220V3",
		.vendor = "CPE220(TP-LINK|UN|N300-2|00000000):3.0\r\n",
		.support_list =
			"SupportList:\r\n"
			"CPE220(TP-LINK|EU|N300-2|00000000):3.0\r\n"
			"CPE220(TP-LINK|EU|N300-2|45550000):3.0\r\n"
			"CPE220(TP-LINK|EU|N300-2|55530000):3.0\r\n"
			"CPE220(TP-LINK|UN|N300-2|00000000):3.0\r\n"
			"CPE220(TP-LINK|UN|N300-2|45550000):3.0\r\n"
			"CPE220(TP-LINK|UN|N300-2|55530000):3.0\r\n"
			"CPE220(TP-LINK|US|N300-2|55530000):3.0\r\n"
			"CPE220(TP-LINK|UN|N300-2):3.0\r\n"
			"CPE220(TP-LINK|EU|N300-2):3.0\r\n"
			"CPE220(TP-LINK|US|N300-2):3.0\r\n",
		.part_trail = 0xff,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"partition-table", 0x20000, 0x02000},
			{"default-mac", 0x30000, 0x00020},
			{"product-info", 0x31100, 0x00100},
			{"device-info", 0x31400, 0x00400},
			{"signature", 0x32000, 0x00400},
			{"device-id", 0x33000, 0x00100},
			{"firmware", 0x40000, 0x770000},
			{"soft-version", 0x7b0000, 0x00100},
			{"support-list", 0x7b1000, 0x01000},
			{"user-config", 0x7c0000, 0x10000},
			{"default-config", 0x7d0000, 0x10000},
			{"log", 0x7e0000, 0x10000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "support-list",
	},

	/** Firmware layout for the CPE510/520 V1 */
	{
		.id     = "CPE510",
		.vendor = "CPE510(TP-LINK|UN|N300-5):1.0\r\n",
		.support_list =
			"SupportList:\r\n"
			"CPE510(TP-LINK|UN|N300-5):1.0\r\n"
			"CPE510(TP-LINK|UN|N300-5):1.1\r\n"
			"CPE510(TP-LINK|UN|N300-5):1.1\r\n"
			"CPE510(TP-LINK|US|N300-5):1.1\r\n"
			"CPE510(TP-LINK|CA|N300-5):1.1\r\n"
			"CPE510(TP-LINK|EU|N300-5):1.1\r\n"
			"CPE520(TP-LINK|UN|N300-5):1.1\r\n"
			"CPE520(TP-LINK|US|N300-5):1.1\r\n"
			"CPE520(TP-LINK|EU|N300-5):1.1\r\n",
		.part_trail = 0xff,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"partition-table", 0x20000, 0x02000},
			{"default-mac", 0x30000, 0x00020},
			{"product-info", 0x31100, 0x00100},
			{"signature", 0x32000, 0x00400},
			{"firmware", 0x40000, 0x770000},
			{"soft-version", 0x7b0000, 0x00100},
			{"support-list", 0x7b1000, 0x00400},
			{"user-config", 0x7c0000, 0x10000},
			{"default-config", 0x7d0000, 0x10000},
			{"log", 0x7e0000, 0x10000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "support-list",
	},

	/** Firmware layout for the CPE510 V2 */
	{
		.id     = "CPE510V2",
		.vendor = "CPE510(TP-LINK|UN|N300-5):2.0\r\n",
		.support_list =
			"SupportList:\r\n"
			"CPE510(TP-LINK|EU|N300-5|00000000):2.0\r\n"
			"CPE510(TP-LINK|EU|N300-5|45550000):2.0\r\n"
			"CPE510(TP-LINK|EU|N300-5|55530000):2.0\r\n"
			"CPE510(TP-LINK|UN|N300-5|00000000):2.0\r\n"
			"CPE510(TP-LINK|UN|N300-5|45550000):2.0\r\n"
			"CPE510(TP-LINK|UN|N300-5|55530000):2.0\r\n"
			"CPE510(TP-LINK|US|N300-5|00000000):2.0\r\n"
			"CPE510(TP-LINK|US|N300-5|45550000):2.0\r\n"
			"CPE510(TP-LINK|US|N300-5|55530000):2.0\r\n"
			"CPE510(TP-LINK|UN|N300-5):2.0\r\n"
			"CPE510(TP-LINK|EU|N300-5):2.0\r\n"
			"CPE510(TP-LINK|US|N300-5):2.0\r\n",
		.part_trail = 0xff,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"partition-table", 0x20000, 0x02000},
			{"default-mac", 0x30000, 0x00020},
			{"product-info", 0x31100, 0x00100},
			{"signature", 0x32000, 0x00400},
			{"firmware", 0x40000, 0x770000},
			{"soft-version", 0x7b0000, 0x00100},
			{"support-list", 0x7b1000, 0x00400},
			{"user-config", 0x7c0000, 0x10000},
			{"default-config", 0x7d0000, 0x10000},
			{"log", 0x7e0000, 0x10000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "support-list",
	},

	/** Firmware layout for the CPE510 V3 */
	{
		.id     = "CPE510V3",
		.vendor = "CPE510(TP-LINK|UN|N300-5):3.0\r\n",
		.support_list =
			"SupportList:\r\n"
			"CPE510(TP-LINK|EU|N300-5|00000000):3.0\r\n"
			"CPE510(TP-LINK|EU|N300-5|45550000):3.0\r\n"
			"CPE510(TP-LINK|EU|N300-5|55530000):3.0\r\n"
			"CPE510(TP-LINK|UN|N300-5|00000000):3.0\r\n"
			"CPE510(TP-LINK|UN|N300-5|45550000):3.0\r\n"
			"CPE510(TP-LINK|UN|N300-5|55530000):3.0\r\n"
			"CPE510(TP-LINK|US|N300-5|00000000):3.0\r\n"
			"CPE510(TP-LINK|US|N300-5|45550000):3.0\r\n"
			"CPE510(TP-LINK|US|N300-5|55530000):3.0\r\n"
			"CPE510(TP-LINK|UN|N300-5):3.0\r\n"
			"CPE510(TP-LINK|EU|N300-5):3.0\r\n"
			"CPE510(TP-LINK|US|N300-5):3.0\r\n"
			"CPE510(TP-LINK|UN|N300-5|00000000):3.20\r\n"
			"CPE510(TP-LINK|US|N300-5|55530000):3.20\r\n"
			"CPE510(TP-LINK|EU|N300-5|45550000):3.20\r\n",
		.part_trail = 0xff,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"partition-table", 0x20000, 0x02000},
			{"default-mac", 0x30000, 0x00020},
			{"product-info", 0x31100, 0x00100},
			{"signature", 0x32000, 0x00400},
			{"firmware", 0x40000, 0x770000},
			{"soft-version", 0x7b0000, 0x00100},
			{"support-list", 0x7b1000, 0x00400},
			{"user-config", 0x7c0000, 0x10000},
			{"default-config", 0x7d0000, 0x10000},
			{"log", 0x7e0000, 0x10000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "support-list",
	},

	/** Firmware layout for the CPE605V1 */
	{
		.id     = "CPE605V1",
		.vendor = "CPE605(TP-LINK|UN|N150-5):1.0\r\n",
		.support_list =
			"SupportList:\r\n"
			"CPE605(TP-LINK|UN|N150-5|00000000):1.0\r\n"
			"CPE605(TP-LINK|EU|N150-5|45550000):1.0\r\n"
			"CPE605(TP-LINK|US|N150-5|55530000):1.0\r\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"partition-table", 0x20000, 0x02000},
			{"default-mac", 0x30000, 0x00020},
			{"serial-number", 0x30100, 0x00020},
			{"product-info", 0x31100, 0x00100},
			{"device-info", 0x31400, 0x00400},
			{"signature", 0x32000, 0x00400},
			{"device-id", 0x33000, 0x00100},
			{"firmware", 0x40000, 0x770000},
			{"soft-version", 0x7b0000, 0x00100},
			{"support-list", 0x7b1000, 0x01000},
			{"user-config", 0x7c0000, 0x10000},
			{"default-config", 0x7d0000, 0x10000},
			{"log", 0x7e0000, 0x10000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "support-list",
	},

	/** Firmware layout for the CPE610V1 */
	{
		.id     = "CPE610V1",
		.vendor = "CPE610(TP-LINK|UN|N300-5|00000000):1.0\r\n",
		.support_list =
			"SupportList:\r\n"
			"CPE610(TP-LINK|EU|N300-5|00000000):1.0\r\n"
			"CPE610(TP-LINK|EU|N300-5|45550000):1.0\r\n"
			"CPE610(TP-LINK|EU|N300-5|55530000):1.0\r\n"
			"CPE610(TP-LINK|UN|N300-5|00000000):1.0\r\n"
			"CPE610(TP-LINK|UN|N300-5|45550000):1.0\r\n"
			"CPE610(TP-LINK|UN|N300-5|55530000):1.0\r\n"
			"CPE610(TP-LINK|US|N300-5|55530000):1.0\r\n"
			"CPE610(TP-LINK|UN|N300-5):1.0\r\n"
			"CPE610(TP-LINK|EU|N300-5):1.0\r\n"
			"CPE610(TP-LINK|US|N300-5):1.0\r\n",
		.part_trail = 0xff,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"partition-table", 0x20000, 0x02000},
			{"default-mac", 0x30000, 0x00020},
			{"product-info", 0x31100, 0x00100},
			{"signature", 0x32000, 0x00400},
			{"firmware", 0x40000, 0x770000},
			{"soft-version", 0x7b0000, 0x00100},
			{"support-list", 0x7b1000, 0x00400},
			{"user-config", 0x7c0000, 0x10000},
			{"default-config", 0x7d0000, 0x10000},
			{"log", 0x7e0000, 0x10000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "support-list",
	},

	/** Firmware layout for the CPE610V2 */
	{
		.id     = "CPE610V2",
		.vendor = "CPE610(TP-LINK|UN|N300-5|00000000):2.0\r\n",
		.support_list =
			"SupportList:\r\n"
			"CPE610(TP-LINK|EU|N300-5|00000000):2.0\r\n"
			"CPE610(TP-LINK|EU|N300-5|45550000):2.0\r\n"
			"CPE610(TP-LINK|EU|N300-5|55530000):2.0\r\n"
			"CPE610(TP-LINK|UN|N300-5|00000000):2.0\r\n"
			"CPE610(TP-LINK|UN|N300-5|45550000):2.0\r\n"
			"CPE610(TP-LINK|UN|N300-5|55530000):2.0\r\n"
			"CPE610(TP-LINK|US|N300-5|55530000):2.0\r\n"
			"CPE610(TP-LINK|UN|N300-5):2.0\r\n"
			"CPE610(TP-LINK|EU|N300-5):2.0\r\n"
			"CPE610(TP-LINK|US|N300-5):2.0\r\n",
		.part_trail = 0xff,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"partition-table", 0x20000, 0x02000},
			{"default-mac", 0x30000, 0x00020},
			{"product-info", 0x31100, 0x00100},
			{"signature", 0x32000, 0x00400},
			{"firmware", 0x40000, 0x770000},
			{"soft-version", 0x7b0000, 0x00100},
			{"support-list", 0x7b1000, 0x00400},
			{"user-config", 0x7c0000, 0x10000},
			{"default-config", 0x7d0000, 0x10000},
			{"log", 0x7e0000, 0x10000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "support-list",
	},
	/** Firmware layout for the CPE710 V1 */
	{
		.id     = "CPE710V1",
		.vendor = "CPE710(TP-LINK|UN|AC866-5|00000000):1.0\r\n",
		.support_list =
			"SupportList:\r\n"
			"CPE710(TP-LINK|UN|AC866-5|00000000):1.0\r\n"
			"CPE710(TP-LINK|EU|AC866-5|45550000):1.0\r\n"
			"CPE710(TP-LINK|US|AC866-5|55530000):1.0\r\n"
			"CPE710(TP-LINK|UN|AC866-5):1.0\r\n"
			"CPE710(TP-LINK|EU|AC866-5):1.0\r\n"
			"CPE710(TP-LINK|US|AC866-5):1.0\r\n",
		.part_trail = 0xff,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x50000},
			{"partition-table", 0x50000, 0x02000},
			{"default-mac", 0x60000, 0x00020},
			{"serial-number", 0x60100, 0x00020},
			{"product-info", 0x61100, 0x00100},
			{"device-info", 0x61400, 0x00400},
			{"signature", 0x62000, 0x00400},
			{"device-id", 0x63000, 0x00100},
			{"firmware", 0x70000, 0xf40000},
			{"soft-version", 0xfb0000, 0x00100},
			{"support-list", 0xfb1000, 0x01000},
			{"user-config", 0xfc0000, 0x10000},
			{"default-config", 0xfd0000, 0x10000},
			{"log", 0xfe0000, 0x10000},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "support-list",
	},

	{
		.id     = "WBS210",
		.vendor = "CPE510(TP-LINK|UN|N300-5):1.0\r\n",
		.support_list =
			"SupportList:\r\n"
			"WBS210(TP-LINK|UN|N300-2):1.20\r\n"
			"WBS210(TP-LINK|US|N300-2):1.20\r\n"
			"WBS210(TP-LINK|EU|N300-2):1.20\r\n",
		.part_trail = 0xff,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"partition-table", 0x20000, 0x02000},
			{"default-mac", 0x30000, 0x00020},
			{"product-info", 0x31100, 0x00100},
			{"signature", 0x32000, 0x00400},
			{"firmware", 0x40000, 0x770000},
			{"soft-version", 0x7b0000, 0x00100},
			{"support-list", 0x7b1000, 0x00400},
			{"user-config", 0x7c0000, 0x10000},
			{"default-config", 0x7d0000, 0x10000},
			{"log", 0x7e0000, 0x10000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "support-list",
	},

	{
		.id     = "WBS210V2",
		.vendor = "CPE510(TP-LINK|UN|N300-5):1.0\r\n",
		.support_list =
			"SupportList:\r\n"
			"WBS210(TP-LINK|UN|N300-2|00000000):2.0\r\n"
			"WBS210(TP-LINK|US|N300-2|55530000):2.0\r\n"
			"WBS210(TP-LINK|EU|N300-2|45550000):2.0\r\n",
		.part_trail = 0xff,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"partition-table", 0x20000, 0x02000},
			{"default-mac", 0x30000, 0x00020},
			{"product-info", 0x31100, 0x00100},
			{"signature", 0x32000, 0x00400},
			{"firmware", 0x40000, 0x770000},
			{"soft-version", 0x7b0000, 0x00100},
			{"support-list", 0x7b1000, 0x00400},
			{"user-config", 0x7c0000, 0x10000},
			{"default-config", 0x7d0000, 0x10000},
			{"log", 0x7e0000, 0x10000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "support-list",
	},

	{
		.id     = "WBS510",
		.vendor = "CPE510(TP-LINK|UN|N300-5):1.0\r\n",
		.support_list =
			"SupportList:\r\n"
			"WBS510(TP-LINK|UN|N300-5):1.20\r\n"
			"WBS510(TP-LINK|US|N300-5):1.20\r\n"
			"WBS510(TP-LINK|EU|N300-5):1.20\r\n"
			"WBS510(TP-LINK|CA|N300-5):1.20\r\n",
		.part_trail = 0xff,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"partition-table", 0x20000, 0x02000},
			{"default-mac", 0x30000, 0x00020},
			{"product-info", 0x31100, 0x00100},
			{"signature", 0x32000, 0x00400},
			{"firmware", 0x40000, 0x770000},
			{"soft-version", 0x7b0000, 0x00100},
			{"support-list", 0x7b1000, 0x00400},
			{"user-config", 0x7c0000, 0x10000},
			{"default-config", 0x7d0000, 0x10000},
			{"log", 0x7e0000, 0x10000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "support-list",
	},

	{
		.id     = "WBS510V2",
		.vendor = "CPE510(TP-LINK|UN|N300-5):1.0\r\n",
		.support_list =
			"SupportList:\r\n"
			"WBS510(TP-LINK|UN|N300-5|00000000):2.0\r\n"
			"WBS510(TP-LINK|US|N300-5|55530000):2.0\r\n"
			"WBS510(TP-LINK|EU|N300-5|45550000):2.0\r\n"
			"WBS510(TP-LINK|CA|N300-5|43410000):2.0\r\n",
		.part_trail = 0xff,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"partition-table", 0x20000, 0x02000},
			{"default-mac", 0x30000, 0x00020},
			{"product-info", 0x31100, 0x00100},
			{"signature", 0x32000, 0x00400},
			{"firmware", 0x40000, 0x770000},
			{"soft-version", 0x7b0000, 0x00100},
			{"support-list", 0x7b1000, 0x00400},
			{"user-config", 0x7c0000, 0x10000},
			{"default-config", 0x7d0000, 0x10000},
			{"log", 0x7e0000, 0x10000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "support-list",
	},

	/** Firmware layout for the AD7200 */
	{
		.id = "AD7200",
		.vendor = "",
		.support_list =
			"SupportList:\r\n"
			"{product_name:AD7200,product_ver:1.0.0,special_id:00000000}\r\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"SBL1", 0x00000, 0x20000},
			{"MIBIB", 0x20000, 0x20000},
			{"SBL2", 0x40000, 0x20000},
			{"SBL3", 0x60000, 0x30000},
			{"DDRCONFIG", 0x90000, 0x10000},
			{"SSD", 0xa0000, 0x10000},
			{"TZ", 0xb0000, 0x30000},
			{"RPM", 0xe0000, 0x20000},
			{"fs-uboot", 0x100000, 0x70000},
			{"uboot-env", 0x170000, 0x40000},
			{"radio", 0x1b0000, 0x40000},
			{"os-image", 0x1f0000, 0x400000},
			{"file-system", 0x5f0000, 0x1900000},
			{"default-mac", 0x1ef0000, 0x00200},
			{"pin", 0x1ef0200, 0x00200},
			{"device-id", 0x1ef0400, 0x00200},
			{"product-info", 0x1ef0600, 0x0fa00},
			{"partition-table", 0x1f00000, 0x10000},
			{"soft-version", 0x1f10000, 0x10000},
			{"support-list", 0x1f20000, 0x10000},
			{"profile", 0x1f30000, 0x10000},
			{"default-config", 0x1f40000, 0x10000},
			{"user-config", 0x1f50000, 0x40000},
			{"qos-db", 0x1f90000, 0x40000},
			{"usb-config", 0x1fd0000, 0x10000},
			{"log", 0x1fe0000, 0x20000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the C2600 */
	{
		.id     = "C2600",
		.vendor = "",
		.support_list =
			"SupportList:\r\n"
			"{product_name:Archer C2600,product_ver:1.0.0,special_id:00000000}\r\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		/**
		    We use a bigger os-image partition than the stock images (and thus
		    smaller file-system), as our kernel doesn't fit in the stock firmware's
		    2 MB os-image since kernel 4.14.
		*/
		.partitions = {
			{"SBL1", 0x00000, 0x20000},
			{"MIBIB", 0x20000, 0x20000},
			{"SBL2", 0x40000, 0x20000},
			{"SBL3", 0x60000, 0x30000},
			{"DDRCONFIG", 0x90000, 0x10000},
			{"SSD", 0xa0000, 0x10000},
			{"TZ", 0xb0000, 0x30000},
			{"RPM", 0xe0000, 0x20000},
			{"fs-uboot", 0x100000, 0x70000},
			{"uboot-env", 0x170000, 0x40000},
			{"radio", 0x1b0000, 0x40000},
			{"os-image", 0x1f0000, 0x400000}, /* Stock: base 0x1f0000 size 0x200000 */
			{"file-system", 0x5f0000, 0x1900000}, /* Stock: base 0x3f0000 size 0x1b00000 */
			{"default-mac", 0x1ef0000, 0x00200},
			{"pin", 0x1ef0200, 0x00200},
			{"product-info", 0x1ef0400, 0x0fc00},
			{"partition-table", 0x1f00000, 0x10000},
			{"soft-version", 0x1f10000, 0x10000},
			{"support-list", 0x1f20000, 0x10000},
			{"profile", 0x1f30000, 0x10000},
			{"default-config", 0x1f40000, 0x10000},
			{"user-config", 0x1f50000, 0x40000},
			{"qos-db", 0x1f90000, 0x40000},
			{"usb-config", 0x1fd0000, 0x10000},
			{"log", 0x1fe0000, 0x20000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the A7-V5 */
	{
		.id     = "ARCHER-A7-V5",
		.support_list =
			"SupportList:\n"
			"{product_name:Archer A7,product_ver:5.0.0,special_id:45550000}\n"
			"{product_name:Archer A7,product_ver:5.0.0,special_id:55530000}\n"
			"{product_name:Archer A7,product_ver:5.0.0,special_id:43410000}\n"
			"{product_name:Archer A7,product_ver:5.0.0,special_id:4A500000}\n"
			"{product_name:Archer A7,product_ver:5.0.0,special_id:54570000}\n"
			"{product_name:Archer A7,product_ver:5.0.0,special_id:52550000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_TEXT("soft_ver:7.0.0\n"),

		/* We're using a dynamic kernel/rootfs split here */
		.partitions = {
			{"factory-boot", 0x00000, 0x20000},
			{"fs-uboot", 0x20000, 0x20000},
			{"firmware", 0x40000, 0xec0000},	/* Stock: name os-image base 0x40000 size 0x120000 */
								/* Stock: name file-system base 0x160000 size 0xda0000 */
			{"default-mac", 0xf40000, 0x00200},
			{"pin", 0xf40200, 0x00200},
			{"device-id", 0xf40400, 0x00100},
			{"product-info", 0xf40500, 0x0fb00},
			{"soft-version", 0xf50000, 0x00100},
			{"extra-para", 0xf51000, 0x01000},
			{"support-list", 0xf52000, 0x0a000},
			{"profile", 0xf5c000, 0x04000},
			{"default-config", 0xf60000, 0x10000},
			{"user-config", 0xf70000, 0x40000},
			{"certificate", 0xfb0000, 0x10000},
			{"partition-table", 0xfc0000, 0x10000},
			{"log", 0xfd0000, 0x20000},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system",
	},

	/** Firmware layout for the Archer A9 v6 */
	{
		.id     = "ARCHER-A9-V6",
		.support_list =
			"SupportList:\n"
			"{product_name:Archer A9,product_ver:6.0,special_id:55530000}\n"
			"{product_name:Archer A9,product_ver:6.0,special_id:45550000}\n"
			"{product_name:Archer A9,product_ver:6.0,special_id:52550000}\n"
			"{product_name:Archer A9,product_ver:6.0,special_id:4A500000}\n"
			"{product_name:Archer C90,product_ver:6.0,special_id:55530000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_TEXT("soft_ver:1.1.0\n"),

		/* We're using a dynamic kernel/rootfs split here */
		.partitions = {
			{"factory-boot", 0x00000, 0x20000},
			{"fs-uboot", 0x20000, 0x20000},
			{"partition-table", 0x40000, 0x10000},
			{"radio", 0x50000, 0x10000},
			{"default-mac", 0x60000, 0x00200},
			{"pin", 0x60200, 0x00200},
			{"device-id", 0x60400, 0x00100},
			{"product-info", 0x60500, 0x0fb00},
			{"soft-version", 0x70000, 0x01000},
			{"extra-para", 0x71000, 0x01000},
			{"support-list", 0x72000, 0x0a000},
			{"profile", 0x7c000, 0x04000},
			{"user-config", 0x80000, 0x10000},
			{"ap-config", 0x90000, 0x10000},
			{"apdef-config", 0xa0000, 0x10000},
			{"router-config", 0xb0000, 0x10000},
			{"firmware", 0xc0000, 0xf00000},	/* Stock: name os-image base 0xc0000 size 0x120000 */
								/* Stock: name file-system base 0x1e0000 size 0xde0000 */
			{"log", 0xfc0000, 0x20000},
			{"certificate", 0xfe0000, 0x10000},
			{"default-config", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system",
	},

	/** Firmware layout for the Archer AX23 v1 */
	{
		.id     = "ARCHER-AX23-V1",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:Archer AX23,product_ver:1.0,special_id:45550000}\n"
			"{product_name:Archer AX23,product_ver:1.0,special_id:4A500000}\n"
			"{product_name:Archer AX23,product_ver:1.0,special_id:4B520000}\n"
			"{product_name:Archer AX23,product_ver:1.0,special_id:52550000}\n"
			"{product_name:Archer AX23,product_ver:1.0.0,special_id:43410000}\n"
			"{product_name:Archer AX23,product_ver:1.0.0,special_id:54570000}\n"
			"{product_name:Archer AX23,product_ver:1.0.0,special_id:55530000}\n"
			"{product_name:Archer AX23,product_ver:1.20,special_id:45550000}\n"
			"{product_name:Archer AX23,product_ver:1.20,special_id:4A500000}\n"
			"{product_name:Archer AX23,product_ver:1.20,special_id:52550000}\n"
			"{product_name:Archer AX23,product_ver:1.20,special_id:55530000}\n"
			"{product_name:Archer AX1800,product_ver:1.20,special_id:45550000}\n"
			"{product_name:Archer AX1800,product_ver:1.20,special_id:52550000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_TEXT("soft_ver:3.0.3\n"),

		.partitions = {
			{"fs-uboot", 0x00000, 0x40000},
			{"firmware", 0x40000, 0xf60000},
			{"default-mac", 0xfa0000, 0x00200},
			{"pin", 0xfa0200, 0x00100},
			{"device-id", 0xfa0300, 0x00100},
			{"product-info", 0xfa0400, 0x0fc00},
			{"default-config", 0xfb0000, 0x08000},
			{"ap-def-config", 0xfb8000, 0x08000},
			{"user-config", 0xfc0000, 0x0a000},
			{"ag-config", 0xfca000, 0x04000},
			{"certificate", 0xfce000, 0x02000},
			{"ap-config", 0xfd0000, 0x06000},
			{"router-config", 0xfd6000, 0x06000},
			{"favicon", 0xfdc000, 0x02000},
			{"logo", 0xfde000, 0x02000},
			{"partition-table", 0xfe0000, 0x00800},
			{"soft-version", 0xfe0800, 0x00100},
			{"support-list", 0xfe0900, 0x00400},
			{"profile", 0xfe0d00, 0x03000},
			{"extra-para", 0xfe3d00, 0x00100},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},
		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system",
	},
	/** Firmware layout for the C2v3 */
	{
		.id     = "ARCHER-C2-V3",
		.support_list =
			"SupportList:\n"
			"{product_name:ArcherC2,product_ver:3.0.0,special_id:00000000}\n"
			"{product_name:ArcherC2,product_ver:3.0.0,special_id:55530000}\n"
			"{product_name:ArcherC2,product_ver:3.0.0,special_id:45550000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_TEXT("soft_ver:3.0.1\n"),

		/** We're using a dynamic kernel/rootfs split here */

		.partitions = {
			{"factory-boot", 0x00000, 0x20000},
			{"fs-uboot", 0x20000, 0x10000},
			{"firmware", 0x30000, 0x7a0000},
			{"user-config", 0x7d0000, 0x04000},
			{"default-mac", 0x7e0000, 0x00100},
			{"device-id", 0x7e0100, 0x00100},
			{"extra-para", 0x7e0200, 0x00100},
			{"pin", 0x7e0300, 0x00100},
			{"support-list", 0x7e0400, 0x00400},
			{"soft-version", 0x7e0800, 0x00400},
			{"product-info", 0x7e0c00, 0x01400},
			{"partition-table", 0x7e2000, 0x01000},
			{"profile", 0x7e3000, 0x01000},
			{"default-config", 0x7e4000, 0x04000},
			{"merge-config", 0x7ec000, 0x02000},
			{"qos-db", 0x7ee000, 0x02000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system",
	},

	/** Firmware layout for the C25v1 */
	{
		.id     = "ARCHER-C25-V1",
		.support_list =
			"SupportList:\n"
			"{product_name:ArcherC25,product_ver:1.0.0,special_id:00000000}\n"
			"{product_name:ArcherC25,product_ver:1.0.0,special_id:55530000}\n"
			"{product_name:ArcherC25,product_ver:1.0.0,special_id:45550000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_TEXT("soft_ver:1.0.0\n"),

		/* We're using a dynamic kernel/rootfs split here */
		.partitions = {
			{"factory-boot", 0x00000, 0x20000},
			{"fs-uboot", 0x20000, 0x10000},
			{"firmware", 0x30000, 0x7a0000},	/* Stock: name os-image base 0x30000 size 0x100000 */
								/* Stock: name file-system base 0x130000 size 0x6a0000 */
			{"user-config", 0x7d0000, 0x04000},
			{"default-mac", 0x7e0000, 0x00100},
			{"device-id", 0x7e0100, 0x00100},
			{"extra-para", 0x7e0200, 0x00100},
			{"pin", 0x7e0300, 0x00100},
			{"support-list", 0x7e0400, 0x00400},
			{"soft-version", 0x7e0800, 0x00400},
			{"product-info", 0x7e0c00, 0x01400},
			{"partition-table", 0x7e2000, 0x01000},
			{"profile", 0x7e3000, 0x01000},
			{"default-config", 0x7e4000, 0x04000},
			{"merge-config", 0x7ec000, 0x02000},
			{"qos-db", 0x7ee000, 0x02000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system",
	},

	/** Firmware layout for the C58v1 */
	{
		.id     = "ARCHER-C58-V1",
		.vendor = "",
		.support_list =
			"SupportList:\r\n"
			"{product_name:Archer C58,product_ver:1.0.0,special_id:00000000}\r\n"
			"{product_name:Archer C58,product_ver:1.0.0,special_id:45550000}\r\n"
			"{product_name:Archer C58,product_ver:1.0.0,special_id:55530000}\r\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_TEXT("soft_ver:1.0.0\n"),

		.partitions = {
			{"fs-uboot", 0x00000, 0x10000},
			{"default-mac", 0x10000, 0x00200},
			{"pin", 0x10200, 0x00200},
			{"product-info", 0x10400, 0x00100},
			{"partition-table", 0x10500, 0x00800},
			{"soft-version", 0x11300, 0x00200},
			{"support-list", 0x11500, 0x00100},
			{"device-id", 0x11600, 0x00100},
			{"profile", 0x11700, 0x03900},
			{"default-config", 0x15000, 0x04000},
			{"user-config", 0x19000, 0x04000},
			{"firmware", 0x20000, 0x7c8000},
			{"certyficate", 0x7e8000, 0x08000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system",
	},

	/** Firmware layout for the C59v1 */
	{
		.id     = "ARCHER-C59-V1",
		.vendor = "",
		.support_list =
			"SupportList:\r\n"
			"{product_name:Archer C59,product_ver:1.0.0,special_id:00000000}\r\n"
			"{product_name:Archer C59,product_ver:1.0.0,special_id:43410000}\r\n"
			"{product_name:Archer C59,product_ver:1.0.0,special_id:45550000}\r\n"
			"{product_name:Archer C59,product_ver:1.0.0,special_id:52550000}\r\n"
			"{product_name:Archer C59,product_ver:1.0.0,special_id:55530000}\r\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_TEXT("soft_ver:1.0.0\n"),

		/* We're using a dynamic kernel/rootfs split here */
		.partitions = {
			{"fs-uboot", 0x00000, 0x10000},
			{"default-mac", 0x10000, 0x00200},
			{"pin", 0x10200, 0x00200},
			{"device-id", 0x10400, 0x00100},
			{"product-info", 0x10500, 0x0fb00},
			{"firmware", 0x20000, 0xe30000},
			{"partition-table", 0xe50000, 0x10000},
			{"soft-version", 0xe60000, 0x10000},
			{"support-list", 0xe70000, 0x10000},
			{"profile", 0xe80000, 0x10000},
			{"default-config", 0xe90000, 0x10000},
			{"user-config", 0xea0000, 0x40000},
			{"usb-config", 0xee0000, 0x10000},
			{"certificate", 0xef0000, 0x10000},
			{"qos-db", 0xf00000, 0x40000},
			{"log", 0xfe0000, 0x10000},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system",
	},

	/** Firmware layout for the C59v2 */
	{
		.id     = "ARCHER-C59-V2",
		.vendor = "",
		.support_list =
			"SupportList:\r\n"
			"{product_name:Archer C59,product_ver:2.0.0,special_id:00000000}\r\n"
			"{product_name:Archer C59,product_ver:2.0.0,special_id:43410000}\r\n"
			"{product_name:Archer C59,product_ver:2.0.0,special_id:45550000}\r\n"
			"{product_name:Archer C59,product_ver:2.0.0,special_id:55530000}\r\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_TEXT("soft_ver:2.0.0 Build 20161206 rel.7303\n"),

		/** We're using a dynamic kernel/rootfs split here */
		.partitions = {
			{"factory-boot", 0x00000, 0x20000},
			{"fs-uboot", 0x20000, 0x10000},
			{"default-mac", 0x30000, 0x00200},
			{"pin", 0x30200, 0x00200},
			{"device-id", 0x30400, 0x00100},
			{"product-info", 0x30500, 0x0fb00},
			{"firmware", 0x40000, 0xe10000},
			{"partition-table", 0xe50000, 0x10000},
			{"soft-version", 0xe60000, 0x10000},
			{"support-list", 0xe70000, 0x10000},
			{"profile", 0xe80000, 0x10000},
			{"default-config", 0xe90000, 0x10000},
			{"user-config", 0xea0000, 0x40000},
			{"usb-config", 0xee0000, 0x10000},
			{"certificate", 0xef0000, 0x10000},
			{"extra-para", 0xf00000, 0x10000},
			{"qos-db", 0xf10000, 0x30000},
			{"log", 0xfe0000, 0x10000},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system",
	},

	/** Firmware layout for the Archer C6 v2 (EU/RU/JP) */
	{
		.id     = "ARCHER-C6-V2",
		.vendor = "",
		.support_list =
			"SupportList:\r\n"
			"{product_name:Archer A6,product_ver:2.0.0,special_id:45550000}\r\n"
			"{product_name:Archer A6,product_ver:2.0.0,special_id:52550000}\r\n"
			"{product_name:Archer C6,product_ver:2.0.0,special_id:45550000}\r\n"
			"{product_name:Archer C6,product_ver:2.0.0,special_id:52550000}\r\n"
			"{product_name:Archer C6,product_ver:2.0.0,special_id:4A500000}\r\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_TEXT("soft_ver:1.9.1\n"),

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"default-mac", 0x20000, 0x00200},
			{"pin", 0x20200, 0x00100},
			{"product-info", 0x20300, 0x00200},
			{"device-id", 0x20500, 0x0fb00},
			{"firmware", 0x30000, 0x7a9400},
			{"soft-version", 0x7d9400, 0x00100},
			{"extra-para", 0x7d9500, 0x00100},
			{"support-list", 0x7d9600, 0x00200},
			{"profile", 0x7d9800, 0x03000},
			{"default-config", 0x7dc800, 0x03000},
			{"partition-table", 0x7df800, 0x00800},
			{"user-config", 0x7e0000, 0x0c000},
			{"certificate", 0x7ec000, 0x04000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system",
	},

	/** Firmware layout for the Archer C6 v2 (US) and A6 v2 (US/TW) */
	{
		.id     = "ARCHER-C6-V2-US",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:Archer A6,product_ver:2.0.0,special_id:55530000}\n"
			"{product_name:Archer A6,product_ver:2.0.0,special_id:54570000}\n"
			"{product_name:Archer C6,product_ver:2.0.0,special_id:55530000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_TEXT("soft_ver:1.9.1\n"),

		.partitions = {
			{"factory-boot", 0x00000, 0x20000},
			{"default-mac", 0x20000, 0x00200},
			{"pin", 0x20200, 0x00100},
			{"product-info", 0x20300, 0x00200},
			{"device-id", 0x20500, 0x0fb00},
			{"fs-uboot", 0x30000, 0x20000},
			{"firmware", 0x50000, 0xf89400},
			{"soft-version", 0xfd9400, 0x00100},
			{"extra-para", 0xfd9500, 0x00100},
			{"support-list", 0xfd9600, 0x00200},
			{"profile", 0xfd9800, 0x03000},
			{"default-config", 0xfdc800, 0x03000},
			{"partition-table", 0xfdf800, 0x00800},
			{"user-config", 0xfe0000, 0x0c000},
			{"certificate", 0xfec000, 0x04000},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},
		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system",
	},
	/** Firmware layout for the Archer C6 v3 */
	{
		.id     = "ARCHER-C6-V3",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:Archer C6,product_ver:3.20,special_id:55530000}"
			"{product_name:Archer C6,product_ver:3.20,special_id:45550000}"
			"{product_name:Archer C6,product_ver:3.20,special_id:52550000}"
			"{product_name:Archer C6,product_ver:3.20,special_id:4A500000}"
			"{product_name:Archer C6,product_ver:3.20,special_id:4B520000}"
			"{product_name:Archer C6,product_ver:3.0.0,special_id:42520000}",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_TEXT("soft_ver:1.0.9\n"),

		.partitions = {
			{"fs-uboot", 0x00000, 0x40000},
			{"firmware", 0x40000, 0xf60000},
			{"default-mac", 0xfa0000, 0x00200},
			{"pin", 0xfa0200, 0x00100},
			{"device-id", 0xfa0300, 0x00100},
			{"product-info", 0xfa0400, 0x0fc00},
			{"default-config", 0xfb0000, 0x08000},
			{"ap-def-config", 0xfb8000, 0x08000},
			{"user-config", 0xfc0000, 0x0a000},
			{"ag-config", 0xfca000, 0x04000},
			{"certificate", 0xfce000, 0x02000},
			{"ap-config", 0xfd0000, 0x06000},
			{"router-config", 0xfd6000, 0x06000},
			{"favicon", 0xfdc000, 0x02000},
			{"logo", 0xfde000, 0x02000},
			{"partition-table", 0xfe0000, 0x00800},
			{"soft-version", 0xfe0800, 0x00100},
			{"support-list", 0xfe0900, 0x00200},
			{"profile", 0xfe0b00, 0x03000},
			{"extra-para", 0xfe3b00, 0x00100},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},
		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system",
	},
	/** Firmware layout for the Archer A6 v3  */
	{
		.id     = "ARCHER-A6-V3",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:Archer A6,product_ver:3.0.0,special_id:43410000}\n"
			"{product_name:Archer A6,product_ver:3.0.0,special_id:55530000}\n"
			"{product_name:Archer A6,product_ver:3.0.0,special_id:54570000}\n"
			"{product_name:Archer A6,product_ver:3.0.0,special_id:4A500000}\n"
			"{product_name:Archer A6,product_ver:3.20,special_id:45550000}\n"
			"{product_name:Archer A6,product_ver:3.20,special_id:52550000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_TEXT("soft_ver:1.0.5\n"),

		.partitions = {
			{"fs-uboot", 0x00000, 0x40000},
			{"firmware", 0x40000, 0xf60000},
			{"default-mac", 0xfa0000, 0x00200},
			{"pin", 0xfa0200, 0x00100},
			{"device-id", 0xfa0300, 0x00100},
			{"product-info", 0xfa0400, 0x0fc00},
			{"default-config", 0xfb0000, 0x08000},
			{"ap-def-config", 0xfb8000, 0x08000},
			{"user-config", 0xfc0000, 0x0a000},
			{"ag-config", 0xfca000, 0x04000},
			{"certificate", 0xfce000, 0x02000},
			{"ap-config", 0xfd0000, 0x06000},
			{"router-config", 0xfd6000, 0x06000},
			{"favicon", 0xfdc000, 0x02000},
			{"logo", 0xfde000, 0x02000},
			{"partition-table", 0xfe0000, 0x00800},
			{"soft-version", 0xfe0800, 0x00100},
			{"support-list", 0xfe0900, 0x00200},
			{"profile", 0xfe0b00, 0x03000},
			{"extra-para", 0xfe3b00, 0x00100},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},
		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system",
	},
	/** Firmware layout for the Archer C6U v1 */
	{
		.id     = "ARCHER-C6U-V1",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:Archer C6U,product_ver:1.0.0,special_id:45550000}\n"
			"{product_name:Archer C6U,product_ver:1.0.0,special_id:52550000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_TEXT("soft_ver:1.0.2\n"),

		.partitions = {
			{"fs-uboot", 0x00000, 0x40000},
			{"firmware", 0x40000, 0xf60000},
			{"default-mac", 0xfa0000, 0x00200},
			{"pin", 0xfa0200, 0x00100},
			{"device-id", 0xfa0300, 0x00100},
			{"product-info", 0xfa0400, 0x0fc00},
			{"default-config", 0xfb0000, 0x08000},
			{"ap-def-config", 0xfb8000, 0x08000},
			{"user-config", 0xfc0000, 0x0c000},
			{"certificate", 0xfcc000, 0x04000},
			{"ap-config", 0xfd0000, 0x08000},
			{"router-config", 0xfd8000, 0x08000},
			{"partition-table", 0xfe0000, 0x00800},
			{"soft-version", 0xfe0800, 0x00100},
			{"support-list", 0xfe0900, 0x00200},
			{"profile", 0xfe0b00, 0x03000},
			{"extra-para", 0xfe3b00, 0x00100},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},
		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system",
	},
	/** Firmware layout for the C60v1 */
	{
		.id     = "ARCHER-C60-V1",
		.vendor = "",
		.support_list =
			"SupportList:\r\n"
			"{product_name:Archer C60,product_ver:1.0.0,special_id:00000000}\r\n"
			"{product_name:Archer C60,product_ver:1.0.0,special_id:43410000}\r\n"
			"{product_name:Archer C60,product_ver:1.0.0,special_id:45550000}\r\n"
			"{product_name:Archer C60,product_ver:1.0.0,special_id:55530000}\r\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_TEXT("soft_ver:1.0.0\n"),

		.partitions = {
			{"fs-uboot", 0x00000, 0x10000},
			{"default-mac", 0x10000, 0x00200},
			{"pin", 0x10200, 0x00200},
			{"product-info", 0x10400, 0x00100},
			{"partition-table", 0x10500, 0x00800},
			{"soft-version", 0x11300, 0x00200},
			{"support-list", 0x11500, 0x00100},
			{"device-id", 0x11600, 0x00100},
			{"profile", 0x11700, 0x03900},
			{"default-config", 0x15000, 0x04000},
			{"user-config", 0x19000, 0x04000},
			{"firmware", 0x20000, 0x7c8000},
			{"certyficate", 0x7e8000, 0x08000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system",
	},

	/** Firmware layout for the C60v2 */
	{
		.id     = "ARCHER-C60-V2",
		.vendor = "",
		.support_list =
			"SupportList:\r\n"
			"{product_name:Archer C60,product_ver:2.0.0,special_id:42520000}\r\n"
			"{product_name:Archer C60,product_ver:2.0.0,special_id:43410000}\r\n"
			"{product_name:Archer C60,product_ver:2.0.0,special_id:45550000}\r\n"
			"{product_name:Archer C60,product_ver:2.0.0,special_id:55530000}\r\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_TEXT("soft_ver:2.0.0\n"),

		.partitions = {
			{"factory-boot", 0x00000, 0x1fb00},
			{"default-mac", 0x1fb00, 0x00200},
			{"pin", 0x1fd00, 0x00100},
			{"product-info", 0x1fe00, 0x00100},
			{"device-id", 0x1ff00, 0x00100},
			{"fs-uboot", 0x20000, 0x10000},
			{"firmware", 0x30000, 0x7a0000},
			{"soft-version", 0x7d9500, 0x00100},
			{"support-list", 0x7d9600, 0x00100},
			{"extra-para", 0x7d9700, 0x00100},
			{"profile", 0x7d9800, 0x03000},
			{"default-config", 0x7dc800, 0x03000},
			{"partition-table", 0x7df800, 0x00800},
			{"user-config", 0x7e0000, 0x0c000},
			{"certificate", 0x7ec000, 0x04000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system",
	},

	/** Firmware layout for the C60v3 */
	{
		.id     = "ARCHER-C60-V3",
		.vendor = "",
		.support_list =
			"SupportList:\r\n"
			"{product_name:Archer C60,product_ver:3.0.0,special_id:42520000}\r\n"
			"{product_name:Archer C60,product_ver:3.0.0,special_id:43410000}\r\n"
			"{product_name:Archer C60,product_ver:3.0.0,special_id:45550000}\r\n"
			"{product_name:Archer C60,product_ver:3.0.0,special_id:55530000}\r\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_TEXT("soft_ver:3.0.0\n"),

		.partitions = {
			{"factory-boot", 0x00000, 0x1fb00},
			{"default-mac", 0x1fb00, 0x00200},
			{"pin", 0x1fd00, 0x00100},
			{"product-info", 0x1fe00, 0x00100},
			{"device-id", 0x1ff00, 0x00100},
			{"fs-uboot", 0x20000, 0x10000},
			{"firmware", 0x30000, 0x7a0000},
			{"soft-version", 0x7d9500, 0x00100},
			{"support-list", 0x7d9600, 0x00100},
			{"extra-para", 0x7d9700, 0x00100},
			{"profile", 0x7d9800, 0x03000},
			{"default-config", 0x7dc800, 0x03000},
			{"partition-table", 0x7df800, 0x00800},
			{"user-config", 0x7e0000, 0x0c000},
			{"certificate", 0x7ec000, 0x04000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system",
	},

	/** Firmware layout for the C5 */
	{
		.id     = "ARCHER-C5-V2",
		.vendor = "",
		.support_list =
			"SupportList:\r\n"
			"{product_name:ArcherC5,product_ver:2.0.0,special_id:00000000}\r\n"
			"{product_name:ArcherC5,product_ver:2.0.0,special_id:55530000}\r\n"
			"{product_name:ArcherC5,product_ver:2.0.0,special_id:4A500000}\r\n", /* JP version */
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x40000},
			{"os-image", 0x40000, 0x200000},
			{"file-system", 0x240000, 0xc00000},
			{"default-mac", 0xe40000, 0x00200},
			{"pin", 0xe40200, 0x00200},
			{"product-info", 0xe40400, 0x00200},
			{"partition-table", 0xe50000, 0x10000},
			{"soft-version", 0xe60000, 0x00200},
			{"support-list", 0xe61000, 0x0f000},
			{"profile", 0xe70000, 0x10000},
			{"default-config", 0xe80000, 0x10000},
			{"user-config", 0xe90000, 0x50000},
			{"log", 0xee0000, 0x100000},
			{"radio_bk", 0xfe0000, 0x10000},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the C7 */
	{
		.id     = "ARCHER-C7-V4",
		.support_list =
			"SupportList:\n"
			"{product_name:Archer C7,product_ver:4.0.0,special_id:00000000}\n"
			"{product_name:Archer C7,product_ver:4.0.0,special_id:41550000}\n"
			"{product_name:Archer C7,product_ver:4.0.0,special_id:45550000}\n"
			"{product_name:Archer C7,product_ver:4.0.0,special_id:4B520000}\n"
			"{product_name:Archer C7,product_ver:4.0.0,special_id:42520000}\n"
			"{product_name:Archer C7,product_ver:4.0.0,special_id:4A500000}\n"
			"{product_name:Archer C7,product_ver:4.0.0,special_id:52550000}\n"
			"{product_name:Archer C7,product_ver:4.0.0,special_id:54570000}\n"
			"{product_name:Archer C7,product_ver:4.0.0,special_id:55530000}\n"
			"{product_name:Archer C7,product_ver:4.0.0,special_id:43410000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_TEXT("soft_ver:1.0.0\n"),

		/* We're using a dynamic kernel/rootfs split here */
		.partitions = {
			{"factory-boot", 0x00000, 0x20000},
			{"fs-uboot", 0x20000, 0x20000},
			{"firmware", 0x40000, 0xEC0000},	/* Stock: name os-image base 0x40000 size 0x120000 */
								/* Stock: name file-system base 0x160000 size 0xda0000 */
			{"default-mac", 0xf00000, 0x00200},
			{"pin", 0xf00200, 0x00200},
			{"device-id", 0xf00400, 0x00100},
			{"product-info", 0xf00500, 0x0fb00},
			{"soft-version", 0xf10000, 0x00100},
			{"extra-para", 0xf11000, 0x01000},
			{"support-list", 0xf12000, 0x0a000},
			{"profile", 0xf1c000, 0x04000},
			{"default-config", 0xf20000, 0x10000},
			{"user-config", 0xf30000, 0x40000},
			{"qos-db", 0xf70000, 0x40000},
			{"certificate", 0xfb0000, 0x10000},
			{"partition-table", 0xfc0000, 0x10000},
			{"log", 0xfd0000, 0x20000},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system",
	},

	/** Firmware layout for the C7 v5*/
	{
		.id     = "ARCHER-C7-V5",
		.support_list =
			"SupportList:\n"
			"{product_name:Archer C7,product_ver:5.0.0,special_id:00000000}\n"
			"{product_name:Archer C7,product_ver:5.0.0,special_id:45550000}\n"
			"{product_name:Archer C7,product_ver:5.0.0,special_id:55530000}\n"
			"{product_name:Archer C7,product_ver:5.0.0,special_id:43410000}\n"
			"{product_name:Archer C7,product_ver:5.0.0,special_id:4A500000}\n"
			"{product_name:Archer C7,product_ver:5.0.0,special_id:54570000}\n"
			"{product_name:Archer C7,product_ver:5.0.0,special_id:52550000}\n"
			"{product_name:Archer C7,product_ver:5.0.0,special_id:4B520000}\n",

		.part_trail = 0x00,
		.soft_ver = SOFT_VER_TEXT("soft_ver:7.0.0\n"),

		/* We're using a dynamic kernel/rootfs split here */
		.partitions = {
			{"factory-boot",    0x00000,  0x20000},
			{"fs-uboot",        0x20000,  0x20000},
			{"partition-table", 0x40000,  0x10000},
			{"radio",           0x50000,  0x10000},
			{"default-mac",     0x60000,  0x00200},
			{"pin",             0x60200,  0x00200},
			{"device-id",       0x60400,  0x00100},
			{"product-info",    0x60500,  0x0fb00},
			{"soft-version",    0x70000,  0x01000},
			{"extra-para",      0x71000,  0x01000},
			{"support-list",    0x72000,  0x0a000},
			{"profile",         0x7c000,  0x04000},
			{"user-config",     0x80000,  0x40000},


			{"firmware",        0xc0000,  0xf00000},	/* Stock: name os-image base 0xc0000  size 0x120000 */
									/* Stock: name file-system base 0x1e0000 size 0xde0000 */

			{"log",             0xfc0000, 0x20000},
			{"certificate",     0xfe0000, 0x10000},
			{"default-config",  0xff0000, 0x10000},
			{NULL, 0, 0}

		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system",
	},

	/** Firmware layout for the C9 */
	{
		.id     = "ARCHERC9",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:ArcherC9,"
			"product_ver:1.0.0,"
			"special_id:00000000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x40000},
			{"os-image", 0x40000, 0x200000},
			{"file-system", 0x240000, 0xc00000},
			{"default-mac", 0xe40000, 0x00200},
			{"pin", 0xe40200, 0x00200},
			{"product-info", 0xe40400, 0x00200},
			{"partition-table", 0xe50000, 0x10000},
			{"soft-version", 0xe60000, 0x00200},
			{"support-list", 0xe61000, 0x0f000},
			{"profile", 0xe70000, 0x10000},
			{"default-config", 0xe80000, 0x10000},
			{"user-config", 0xe90000, 0x50000},
			{"log", 0xee0000, 0x100000},
			{"radio_bk", 0xfe0000, 0x10000},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the Deco M4R v1 and v2 */
	{
		.id     = "DECO-M4R-V1",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:M4R,product_ver:1.0.0,special_id:55530000}\n"
			"{product_name:M4R,product_ver:1.0.0,special_id:45550000}\n"
			"{product_name:M4R,product_ver:1.0.0,special_id:43410000}\n"
			"{product_name:M4R,product_ver:1.0.0,special_id:4A500000}\n"
			"{product_name:M4R,product_ver:1.0.0,special_id:41550000}\n"
			"{product_name:M4R,product_ver:1.0.0,special_id:4B520000}\n"
			"{product_name:M4R,product_ver:1.0.0,special_id:49440000}\n"
			"{product_name:M4R,product_ver:2.0.0,special_id:55530000}\n"
			"{product_name:M4R,product_ver:2.0.0,special_id:45550000}\n"
			"{product_name:M4R,product_ver:2.0.0,special_id:43410000}\n"
			"{product_name:M4R,product_ver:2.0.0,special_id:4A500000}\n"
			"{product_name:M4R,product_ver:2.0.0,special_id:41550000}\n"
			"{product_name:M4R,product_ver:2.0.0,special_id:4B520000}\n"
			"{product_name:M4R,product_ver:2.0.0,special_id:54570000}\n"
			"{product_name:M4R,product_ver:2.0.0,special_id:42340000}\n"
			"{product_name:M4R,product_ver:2.0.0,special_id:49440000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x80000},
			{"firmware", 0x80000, 0xe00000},
			{"product-info", 0xe80000, 0x05000},
			{"default-mac", 0xe85000, 0x01000},
			{"device-id", 0xe86000, 0x01000},
			{"support-list", 0xe87000, 0x10000},
			{"user-config", 0xea7000, 0x10000},
			{"device-config", 0xeb7000, 0x10000},
			{"group-info", 0xec7000, 0x10000},
			{"partition-table", 0xed7000, 0x02000},
			{"soft-version", 0xed9000, 0x10000},
			{"profile", 0xee9000, 0x10000},
			{"default-config", 0xef9000, 0x10000},
			{"url-sig", 0xfe0000, 0x10000},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},
		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system",
	},

	/** Firmware layout for the Deco M4R v4 */
	{
		.id     = "DECO-M4R-V4",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:M4R,product_ver:4.0.0,special_id:55530000}\n"
			"{product_name:M4R,product_ver:4.0.0,special_id:45550000}\n"
			"{product_name:M4R,product_ver:4.0.0,special_id:4A500000}\n"
			"{product_name:M4R,product_ver:4.0.0,special_id:42340000}\n"
			"{product_name:M4R,product_ver:4.0.0,special_id:5A470000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_TEXT("soft_ver:1.0.0\n"),

		.partitions = {
			{"fs-uboot", 0x00000, 0x40000},
			{"firmware", 0x40000, 0xf60000},
			{"default-mac", 0xfa0000, 0x00300},
			{"device-id", 0xfa0300, 0x00100},
			{"product-info", 0xfa0400, 0x0fc00},
			{"group-info", 0xfb0000, 0x04000},
			{"user-config", 0xfb4000, 0x0c000},
			{"device-config", 0xfc0000, 0x10000},
			{"default-config", 0xfd0000, 0x10000},
			{"partition-table", 0xfe0000, 0x00800},
			{"soft-version", 0xfe0800, 0x00100},
			{"support-list", 0xfe0900, 0x00200},
			{"profile", 0xfe0b00, 0x03000},
			{"extra-para", 0xfe3b00, 0x00100},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},
		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system",
	},

	/** Firmware layout for the Deco M5 */
	{
		.id = "DECO-M5",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:M5,product_ver:1.0.0,special_id:55530000}\n"
			"{product_name:M5,product_ver:1.0.0,special_id:45550000}\n"
			"{product_name:M5,product_ver:1.0.0,special_id:43410000}\n"
			"{product_name:M5,product_ver:1.0.0,special_id:4A500000}\n"
			"{product_name:M5,product_ver:1.0.0,special_id:41550000}\n"
			"{product_name:M5,product_ver:1.0.0,special_id:4B520000}\n"
			"{product_name:M5,product_ver:1.0.0,special_id:49440000}\n"
			"{product_name:M5,product_ver:3.0.0,special_id:55530000}\n"
			"{product_name:M5,product_ver:3.0.0,special_id:45550000}\n"
			"{product_name:M5,product_ver:3.0.0,special_id:43410000}\n"
			"{product_name:M5,product_ver:3.0.0,special_id:4A500000}\n"
			"{product_name:M5,product_ver:3.0.0,special_id:41550000}\n"
			"{product_name:M5,product_ver:3.0.0,special_id:4B520000}\n"
			"{product_name:M5,product_ver:3.0.0,special_id:49440000}\n"
			"{product_name:M5,product_ver:3.0.0,special_id:53570000}\n"
			"{product_name:M5,product_ver:3.0.0,special_id:42340000}\n"
			"{product_name:M5,product_ver:3.0.0,special_id:54570000}\n"
			"{product_name:M5,product_ver:3.2.0,special_id:55530000}\n"
			"{product_name:M5,product_ver:3.2.0,special_id:45550000}\n"
			"{product_name:M5,product_ver:3.2.0,special_id:43410000}\n"
			"{product_name:M5,product_ver:3.2.0,special_id:4A500000}\n"
			"{product_name:M5,product_ver:3.2.0,special_id:41550000}\n"
			"{product_name:M5,product_ver:3.2.0,special_id:4B520000}\n"
			"{product_name:M5,product_ver:3.2.0,special_id:49440000}\n"
			"{product_name:M5,product_ver:3.2.0,special_id:53570000}\n"
			"{product_name:M5,product_ver:3.2.0,special_id:42340000}\n"
			"{product_name:M5,product_ver:3.2.0,special_id:54570000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"SBL1", 0x00000, 0x30000},
			{"boot-config_0", 0x30000, 0x10000},
			{"MIBIB", 0x40000, 0x10000},
			{"boot-config_1", 0x50000, 0x10000},
			{"QSEE", 0x60000, 0x60000},
			{"CDT", 0xc0000, 0x10000},
			{"DDRPARAMS", 0xd0000, 0x10000},
			{"uboot-env", 0xe0000, 0x10000},
			{"fs-uboot@0", 0xf0000, 0x80000},
			{"radio", 0x170000, 0x0fff0},
			{"bluetooth-XTAL", 0x17fff0, 0x00010},
			{"default-mac", 0x180000, 0x01000},
			{"device-id", 0x182000, 0x01000},
			{"product-info", 0x183000, 0x05000},
			{"support-list", 0x190000, 0x10000},
			{"user-config", 0x200000, 0x10000},
			{"device-config", 0x210000, 0x10000},
			{"group-info", 0x220000, 0x10000},
			{"partition-table@0", 0x230000, 0x02000},
			{"os-image@0", 0x240000, 0x300000},
			{"file-system@0", 0x540000, 0x790000},
			{"soft-version@0", 0xcd0000, 0x10000},
			{"profile@0", 0xce0000, 0x10000},
			{"default-config@0", 0xcf0000, 0x10000},
			{"partition-table@1", 0xd00000, 0x02000},
			{"fs-uboot@1", 0xd10000, 0x80000},
			{"os-image@1", 0xd90000, 0x400000},
			{"file-system@1", 0x1190000, 0xc40000},
			{"soft-version@1", 0x1dd0000, 0x10000},
			{"profile@1", 0x1de0000, 0x10000},
			{"default-config@1", 0x1df0000, 0x10000},
			{"tm-sig", 0x1e00000, 0x200000},
			{NULL, 0, 0}
		},

		.partition_names.partition_table = "partition-table@1",
		.partition_names.soft_ver = "soft-version@1",
		.partition_names.os_image = "os-image@1",
		.partition_names.file_system = "file-system@1",

		.first_sysupgrade_partition = "os-image@1",
		.last_sysupgrade_partition = "file-system@1"
	},

	/** Firmware layout for the Deco S4 v2 */
	{
		.id     = "DECO-S4-V2",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:S4,product_ver:1.0.0,special_id:55530000}\n"
			"{product_name:S4,product_ver:1.0.0,special_id:45550000}\n"
			"{product_name:S4,product_ver:1.0.0,special_id:43410000}\n"
			"{product_name:S4,product_ver:1.0.0,special_id:4A500000}\n"
			"{product_name:S4,product_ver:1.0.0,special_id:41550000}\n"
			"{product_name:S4,product_ver:1.0.0,special_id:4B520000}\n"
			"{product_name:S4,product_ver:2.0.0,special_id:55530000}\n"
			"{product_name:S4,product_ver:2.0.0,special_id:45550000}\n"
			"{product_name:S4,product_ver:2.0.0,special_id:43410000}\n"
			"{product_name:S4,product_ver:2.0.0,special_id:4A500000}\n"
			"{product_name:S4,product_ver:2.0.0,special_id:41550000}\n"
			"{product_name:S4,product_ver:2.0.0,special_id:4B520000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x80000},
			{"product-info", 0x80000, 0x05000},
			{"default-mac", 0x85000, 0x01000},
			{"device-id", 0x86000, 0x01000},
			{"support-list", 0x87000, 0x10000},
			{"user-config", 0xa7000, 0x10000},
			{"device-config", 0xb7000, 0x10000},
			{"group-info", 0xc7000, 0x10000},
			{"partition-table", 0xd7000, 0x02000},
			{"soft-version", 0xd9000, 0x10000},
			{"profile", 0xe9000, 0x10000},
			{"default-config", 0xf9000, 0x10000},
			{"url-sig", 0x1e0000, 0x10000},
			{"radio", 0x1f0000, 0x10000},
			{"firmware", 0x200000, 0xe00000},
			{NULL, 0, 0}
		},
		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system",
	},

	/** Firmware layout for the EAP120 */
	{
		.id     = "EAP120",
		.vendor = "EAP120(TP-LINK|UN|N300-2):1.0\r\n",
		.support_list =
			"SupportList:\r\n"
			"EAP120(TP-LINK|UN|N300-2):1.0\r\n",
		.part_trail = 0xff,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"partition-table", 0x20000, 0x02000},
			{"default-mac", 0x30000, 0x00020},
			{"support-list", 0x31000, 0x00100},
			{"product-info", 0x31100, 0x00100},
			{"soft-version", 0x32000, 0x00100},
			{"os-image", 0x40000, 0x180000},
			{"file-system", 0x1c0000, 0x600000},
			{"user-config", 0x7c0000, 0x10000},
			{"backup-config", 0x7d0000, 0x10000},
			{"log", 0x7e0000, 0x10000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the EAP225-Outdoor v1 */
	{
		.id     = "EAP225-OUTDOOR-V1",
		.support_list =
			"SupportList:\r\n"
			"EAP225-Outdoor(TP-Link|UN|AC1200-D):1.0\r\n",
		.part_trail = PART_TRAIL_NONE,
		.soft_ver = SOFT_VER_DEFAULT,
		.soft_ver_compat_level = 1,

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"partition-table", 0x20000, 0x02000},
			{"default-mac", 0x30000, 0x01000},
			{"support-list", 0x31000, 0x00100},
			{"product-info", 0x31100, 0x00400},
			{"soft-version", 0x32000, 0x00100},
			{"firmware", 0x40000, 0xd80000},
			{"user-config", 0xdc0000, 0x30000},
			{"mutil-log", 0xf30000, 0x80000},
			{"oops", 0xfb0000, 0x40000},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the EAP225 v1 */
	{
		.id     = "EAP225-V1",
		.support_list =
			"SupportList:\r\n"
			"EAP225(TP-LINK|UN|AC1200-D):1.0\r\n",
		.part_trail = PART_TRAIL_NONE,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"partition-table", 0x20000, 0x02000},
			{"default-mac", 0x30000, 0x01000},
			{"support-list", 0x31000, 0x00100},
			{"product-info", 0x31100, 0x00400},
			{"soft-version", 0x32000, 0x00100},
			{"firmware", 0x40000, 0xd80000},
			{"user-config", 0xdc0000, 0x30000},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the EAP225 v3
	 * Also compatible with:
	 *   - EAP225 v3.20
	 *   - EAP225 v4
	 *   - EAP225-Outdoor v1
	 *   - EAP225-Outdoor v3
	 *   */
	{
		.id     = "EAP225-V3",
		.support_list =
			"SupportList:\r\n"
			"EAP225(TP-Link|UN|AC1350-D):3.0\r\n"
			"EAP225(TP-Link|UN|AC1350-D):3.20\r\n"
			"EAP225(TP-Link|UN|AC1350-D):4.0 CA\r\n"
			"EAP225-Outdoor(TP-Link|UN|AC1200-D):1.0\r\n"
			"EAP225-Outdoor(TP-Link|UN|AC1200-D):3.0 CA,JP\r\n",
		.part_trail = PART_TRAIL_NONE,
		.soft_ver = SOFT_VER_DEFAULT,
		.soft_ver_compat_level = 1,

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"partition-table", 0x20000, 0x02000},
			{"default-mac", 0x30000, 0x01000},
			{"support-list", 0x31000, 0x00100},
			{"product-info", 0x31100, 0x00400},
			{"soft-version", 0x32000, 0x00100},
			{"firmware", 0x40000, 0xd80000},
			{"user-config", 0xdc0000, 0x30000},
			{"mutil-log", 0xf30000, 0x80000},
			{"oops", 0xfb0000, 0x40000},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the EAP225-Wall v2 */
	{
		.id     = "EAP225-WALL-V2",
		.support_list =
			"SupportList:\r\n"
			"EAP225-Wall(TP-Link|UN|AC1200-D):2.0\r\n",
		.part_trail = PART_TRAIL_NONE,
		.soft_ver = SOFT_VER_DEFAULT,
		.soft_ver_compat_level = 1,

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"partition-table", 0x20000, 0x02000},
			{"default-mac", 0x30000, 0x01000},
			{"support-list", 0x31000, 0x00100},
			{"product-info", 0x31100, 0x00400},
			{"soft-version", 0x32000, 0x00100},
			{"firmware", 0x40000, 0xd80000},
			{"user-config", 0xdc0000, 0x30000},
			{"mutil-log", 0xf30000, 0x80000},
			{"oops", 0xfb0000, 0x40000},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the EAP235-Wall v1 */
	{
		.id     = "EAP235-WALL-V1",
		.support_list =
			"SupportList:\r\n"
			"EAP235-Wall(TP-Link|UN|AC1200-D):1.0\r\n",
		.part_trail = PART_TRAIL_NONE,
		.soft_ver = SOFT_VER_NUMERIC(3, 0, 0),
		.soft_ver_compat_level = 1,

		.partitions = {
			{"fs-uboot", 0x00000, 0x80000},
			{"partition-table", 0x80000, 0x02000},
			{"default-mac", 0x90000, 0x01000},
			{"support-list", 0x91000, 0x00100},
			{"product-info", 0x91100, 0x00400},
			{"soft-version", 0x92000, 0x00100},
			{"firmware", 0xa0000, 0xd20000},
			{"user-config", 0xdc0000, 0x30000},
			{"mutil-log", 0xf30000, 0x80000},
			{"oops", 0xfb0000, 0x40000},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the EAP245 v1 */
	{
		.id     = "EAP245-V1",
		.support_list =
			"SupportList:\r\n"
			"EAP245(TP-LINK|UN|AC1750-D):1.0\r\n",
		.part_trail = PART_TRAIL_NONE,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"partition-table", 0x20000, 0x02000},
			{"default-mac", 0x30000, 0x01000},
			{"support-list", 0x31000, 0x00100},
			{"product-info", 0x31100, 0x00400},
			{"soft-version", 0x32000, 0x00100},
			{"firmware", 0x40000, 0xd80000},
			{"user-config", 0xdc0000, 0x30000},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the EAP245 v3 */
	{
		.id     = "EAP245-V3",
		.support_list =
			"SupportList:\r\n"
			"EAP245(TP-Link|UN|AC1750-D):3.0\r\n"
			"EAP265 HD(TP-Link|UN|AC1750-D):1.0",
		.part_trail = PART_TRAIL_NONE,
		.soft_ver = SOFT_VER_DEFAULT,
		.soft_ver_compat_level = 1,

		/** Firmware partition with dynamic kernel/rootfs split */
		.partitions = {
			{"factroy-boot", 0x00000, 0x40000},
			{"fs-uboot", 0x40000, 0x40000},
			{"partition-table", 0x80000, 0x10000},
			{"default-mac", 0x90000, 0x01000},
			{"support-list", 0x91000, 0x00100},
			{"product-info", 0x91100, 0x00400},
			{"soft-version", 0x92000, 0x00100},
			{"radio", 0xa0000, 0x10000},
			{"extra-para", 0xb0000, 0x10000},
			{"firmware", 0xc0000, 0xe40000},
			{"config", 0xf00000, 0x30000},
			{"mutil-log", 0xf30000, 0x80000},
			{"oops", 0xfb0000, 0x40000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the EAP610 v3/EAP613 v1 */
	{
		.id = "EAP610-V3",
		.soft_ver = SOFT_VER_DEFAULT,
		.soft_ver_compat_level = 1,
		.support_list =
			"SupportList:\r\n"
			"EAP610(TP-Link|UN|AX1800-D):3.0\r\n"
			"EAP610(TP-Link|JP|AX1800-D):3.0\r\n"
			"EAP610(TP-Link|EG|AX1800-D):3.0\r\n"
			"EAP610(TP-Link|CA|AX1800-D):3.0\r\n"
			"EAP613(TP-Link|UN|AX1800-D):1.0 JP\r\n",
		.part_trail = PART_TRAIL_NONE,

		.partitions = {
			{"fs-uboot", 0x00000, 0x80000},
			{"partition-table", 0x80000, 0x02000},
			{"default-mac", 0x90000, 0x01000},
			{"support-list", 0x91000, 0x00100},
			{"product-info", 0x91100, 0x00400},
			{"soft-version", 0x92000, 0x00100},
			{"firmware", 0xa0000, 0xcf0000},
			{"user-config", 0xd90000, 0x60000},
			{"mutil-log", 0xf30000, 0x80000},
			{"oops", 0xfb0000, 0x40000},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the EAP615-Wall v1 */
	{
		.id = "EAP615-WALL-V1",
		.soft_ver = SOFT_VER_DEFAULT,
		.soft_ver_compat_level = 2,
		.support_list =
			"SupportList:\r\n"
			"EAP615-Wall(TP-Link|UN|AX1800-D):1.0\r\n"
			"EAP615-Wall(TP-Link|CA|AX1800-D):1.0\r\n"
			"EAP615-Wall(TP-Link|JP|AX1800-D):1.0\r\n",
		.part_trail = PART_TRAIL_NONE,

		.partitions = {
			{"fs-uboot", 0x00000, 0x80000},
			{"partition-table", 0x80000, 0x02000},
			{"default-mac", 0x90000, 0x01000},
			{"support-list", 0x91000, 0x00100},
			{"product-info", 0x91100, 0x00400},
			{"soft-version", 0x92000, 0x00100},
			{"firmware", 0xa0000, 0xcf0000},
			{"user-config", 0xd90000, 0x60000},
			{"mutil-log", 0xf30000, 0x80000},
			{"oops", 0xfb0000, 0x40000},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the TL-WA1201 v2 */
	{
		.id     = "TL-WA1201-V2",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:TL-WA1201,product_ver:2.0.0,special_id:45550000}\n"
			"{product_name:TL-WA1201,product_ver:2.0.0,special_id:55530000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_TEXT("soft_ver:1.0.1 Build 20200709 rel.66244\n"),

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"default-mac", 0x20000, 0x00200},
			{"pin", 0x20200, 0x00100},
			{"product-info", 0x20300, 0x00200},
			{"device-id", 0x20500, 0x0fb00},
			{"firmware", 0x30000, 0xce0000},
			{"portal-logo", 0xd10000, 0x20000},
			{"portal-back", 0xd30000, 0x200000},
			{"soft-version", 0xf30000, 0x00200},
			{"extra-para", 0xf30200, 0x00200},
			{"support-list", 0xf30400, 0x00200},
			{"profile", 0xf30600, 0x0fa00},
			{"apdef-config", 0xf40000, 0x10000},
			{"ap-config", 0xf50000, 0x10000},
			{"redef-config", 0xf60000, 0x10000},
			{"re-config", 0xf70000, 0x10000},
			{"multidef-config", 0xf80000, 0x10000},
			{"multi-config", 0xf90000, 0x10000},
			{"clientdef-config", 0xfa0000, 0x10000},
			{"client-config", 0xfb0000, 0x10000},
			{"partition-table", 0xfc0000, 0x10000},
			{"user-config", 0xfd0000, 0x10000},
			{"certificate", 0xfe0000, 0x10000},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},
		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system",
	},

	/** Firmware layout for the TL-WA850RE v2 */
	{
		.id     = "TLWA850REV2",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:TL-WA850RE,product_ver:2.0.0,special_id:55530000}\n"
			"{product_name:TL-WA850RE,product_ver:2.0.0,special_id:00000000}\n"
			"{product_name:TL-WA850RE,product_ver:2.0.0,special_id:55534100}\n"
			"{product_name:TL-WA850RE,product_ver:2.0.0,special_id:45550000}\n"
			"{product_name:TL-WA850RE,product_ver:2.0.0,special_id:4B520000}\n"
			"{product_name:TL-WA850RE,product_ver:2.0.0,special_id:42520000}\n"
			"{product_name:TL-WA850RE,product_ver:2.0.0,special_id:4A500000}\n"
			"{product_name:TL-WA850RE,product_ver:2.0.0,special_id:43410000}\n"
			"{product_name:TL-WA850RE,product_ver:2.0.0,special_id:41550000}\n"
			"{product_name:TL-WA850RE,product_ver:2.0.0,special_id:52550000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		/**
		   576KB were moved from file-system to os-image
		   in comparison to the stock image
		*/
		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"firmware", 0x20000, 0x390000},
			{"partition-table", 0x3b0000, 0x02000},
			{"default-mac", 0x3c0000, 0x00020},
			{"pin", 0x3c0100, 0x00020},
			{"product-info", 0x3c1000, 0x01000},
			{"soft-version", 0x3c2000, 0x00100},
			{"support-list", 0x3c3000, 0x01000},
			{"profile", 0x3c4000, 0x08000},
			{"user-config", 0x3d0000, 0x10000},
			{"default-config", 0x3e0000, 0x10000},
			{"radio", 0x3f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the TL-WA855RE v1 */
	{
		.id     = "TLWA855REV1",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:TL-WA855RE,product_ver:1.0.0,special_id:00000000}\n"
			"{product_name:TL-WA855RE,product_ver:1.0.0,special_id:55530000}\n"
			"{product_name:TL-WA855RE,product_ver:1.0.0,special_id:45550000}\n"
			"{product_name:TL-WA855RE,product_ver:1.0.0,special_id:4B520000}\n"
			"{product_name:TL-WA855RE,product_ver:1.0.0,special_id:42520000}\n"
			"{product_name:TL-WA855RE,product_ver:1.0.0,special_id:4A500000}\n"
			"{product_name:TL-WA855RE,product_ver:1.0.0,special_id:43410000}\n"
			"{product_name:TL-WA855RE,product_ver:1.0.0,special_id:41550000}\n"
			"{product_name:TL-WA855RE,product_ver:1.0.0,special_id:52550000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"os-image", 0x20000, 0x150000},
			{"file-system", 0x170000, 0x240000},
			{"partition-table", 0x3b0000, 0x02000},
			{"default-mac", 0x3c0000, 0x00020},
			{"pin", 0x3c0100, 0x00020},
			{"product-info", 0x3c1000, 0x01000},
			{"soft-version", 0x3c2000, 0x00100},
			{"support-list", 0x3c3000, 0x01000},
			{"profile", 0x3c4000, 0x08000},
			{"user-config", 0x3d0000, 0x10000},
			{"default-config", 0x3e0000, 0x10000},
			{"radio", 0x3f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the TL-WPA8630P v2 (EU)*/
	{
		.id     = "TL-WPA8630P-V2.0-EU",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:TL-WPA8630P,product_ver:2.0.0,special_id:45550000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"factory-uboot", 0x00000, 0x20000},
			{"fs-uboot", 0x20000, 0x20000},
			{"firmware", 0x40000, 0x5e0000},
			{"partition-table", 0x620000, 0x02000},
			{"default-mac", 0x630000, 0x00020},
			{"pin", 0x630100, 0x00020},
			{"device-id", 0x630200, 0x00030},
			{"product-info", 0x631100, 0x01000},
			{"extra-para", 0x632100, 0x01000},
			{"soft-version", 0x640000, 0x01000},
			{"support-list", 0x641000, 0x01000},
			{"profile", 0x642000, 0x08000},
			{"user-config", 0x650000, 0x10000},
			{"default-config", 0x660000, 0x10000},
			{"default-nvm", 0x670000, 0xc0000},
			{"default-pib", 0x730000, 0x40000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the TL-WPA8630P v2 (INT)*/
	{
		.id     = "TL-WPA8630P-V2-INT",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:TL-WPA8630P,product_ver:2.0.0,special_id:41550000}\n"
			"{product_name:TL-WPA8630P,product_ver:2.0.0,special_id:44450000}\n"
			"{product_name:TL-WPA8630P,product_ver:2.1.0,special_id:41550000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"factory-uboot", 0x00000, 0x20000},
			{"fs-uboot", 0x20000, 0x20000},
			{"firmware", 0x40000, 0x5e0000},
			{"partition-table", 0x620000, 0x02000},
			{"extra-para", 0x632100, 0x01000},
			{"soft-version", 0x640000, 0x01000},
			{"support-list", 0x641000, 0x01000},
			{"profile", 0x642000, 0x08000},
			{"user-config", 0x650000, 0x10000},
			{"default-config", 0x660000, 0x10000},
			{"default-nvm", 0x670000, 0xc0000},
			{"default-pib", 0x730000, 0x40000},
			{"default-mac", 0x7e0000, 0x00020},
			{"pin", 0x7e0100, 0x00020},
			{"device-id", 0x7e0200, 0x00030},
			{"product-info", 0x7e1100, 0x01000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the TL-WPA8630P v2.1 (EU)*/
	{
		.id     = "TL-WPA8630P-V2.1-EU",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:TL-WPA8630P,product_ver:2.1.0,special_id:45550000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"factory-uboot", 0x00000, 0x20000},
			{"fs-uboot", 0x20000, 0x20000},
			{"firmware", 0x40000, 0x5e0000},
			{"extra-para", 0x680000, 0x01000},
			{"product-info", 0x690000, 0x01000},
			{"partition-table", 0x6a0000, 0x02000},
			{"soft-version", 0x6b0000, 0x01000},
			{"support-list", 0x6b1000, 0x01000},
			{"profile", 0x6b2000, 0x08000},
			{"user-config", 0x6c0000, 0x10000},
			{"default-config", 0x6d0000, 0x10000},
			{"default-nvm", 0x6e0000, 0xc0000},
			{"default-pib", 0x7a0000, 0x40000},
			{"default-mac", 0x7e0000, 0x00020},
			{"pin", 0x7e0100, 0x00020},
			{"device-id", 0x7e0200, 0x00030},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the TL-WPA8631P v3 */
	{
		.id     = "TL-WPA8631P-V3",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:TL-WPA8631P,product_ver:3.0.0,special_id:41550000}\n"
			"{product_name:TL-WPA8631P,product_ver:3.0.0,special_id:45550000}\n"
			"{product_name:TL-WPA8631P,product_ver:3.0.0,special_id:55530000}\n"
			"{product_name:TL-WPA8631P,product_ver:4.0.0,special_id:41550000}\n"
			"{product_name:TL-WPA8631P,product_ver:4.0.0,special_id:45550000}\n"
			"{product_name:TL-WPA8631P,product_ver:4.0.0,special_id:55530000}\n"
			"{product_name:TL-WPA8635P,product_ver:3.0.0,special_id:46520000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"firmware", 0x20000, 0x710000},
			{"partition-table", 0x730000, 0x02000},
			{"default-mac", 0x732000, 0x00020},
			{"pin", 0x732100, 0x00020},
			{"device-id", 0x732200, 0x00030},
			{"default-region", 0x732300, 0x00010},
			{"product-info", 0x732400, 0x00200},
			{"extra-para", 0x732600, 0x00200},
			{"soft-version", 0x732800, 0x00100},
			{"support-list", 0x732900, 0x00200},
			{"profile", 0x732b00, 0x00100},
			{"default-config", 0x732c00, 0x00800},
			{"plc-type", 0x733400, 0x00020},
			{"default-pib", 0x733500, 0x06000},
			{"user-config", 0x740000, 0x10000},
			{"plc-pib", 0x750000, 0x10000},
			{"plc-nvm", 0x760000, 0x90000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the TL-WR1043 v5 */
	{
		.id     = "TLWR1043NV5",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:TL-WR1043N,product_ver:5.0.0,special_id:45550000}\n"
			"{product_name:TL-WR1043N,product_ver:5.0.0,special_id:55530000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_TEXT("soft_ver:1.0.0\n"),
		.partitions = {
			{"factory-boot", 0x00000, 0x20000},
			{"fs-uboot", 0x20000, 0x20000},
			{"firmware", 0x40000, 0xec0000},
			{"default-mac", 0xf00000, 0x00200},
			{"pin", 0xf00200, 0x00200},
			{"device-id", 0xf00400, 0x00100},
			{"product-info", 0xf00500, 0x0fb00},
			{"soft-version", 0xf10000, 0x01000},
			{"extra-para", 0xf11000, 0x01000},
			{"support-list", 0xf12000, 0x0a000},
			{"profile", 0xf1c000, 0x04000},
			{"default-config", 0xf20000, 0x10000},
			{"user-config", 0xf30000, 0x40000},
			{"qos-db", 0xf70000, 0x40000},
			{"certificate", 0xfb0000, 0x10000},
			{"partition-table", 0xfc0000, 0x10000},
			{"log", 0xfd0000, 0x20000},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},
		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the TL-WR1043 v4 */
	{
		.id     = "TLWR1043NDV4",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:TL-WR1043ND,product_ver:4.0.0,special_id:45550000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		/* We're using a dynamic kernel/rootfs split here */
		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"firmware", 0x20000, 0xf30000},
			{"default-mac", 0xf50000, 0x00200},
			{"pin", 0xf50200, 0x00200},
			{"product-info", 0xf50400, 0x0fc00},
			{"soft-version", 0xf60000, 0x0b000},
			{"support-list", 0xf6b000, 0x04000},
			{"profile", 0xf70000, 0x04000},
			{"default-config", 0xf74000, 0x0b000},
			{"user-config", 0xf80000, 0x40000},
			{"partition-table", 0xfc0000, 0x10000},
			{"log", 0xfd0000, 0x20000},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the TL-WR902AC v1 */
	{
		.id     = "TL-WR902AC-V1",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:TL-WR902AC,product_ver:1.0.0,special_id:45550000}\n"
			"{product_name:TL-WR902AC,product_ver:1.0.0,special_id:55530000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		/**
		   384KB were moved from file-system to os-image
		   in comparison to the stock image
		*/
		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"firmware", 0x20000, 0x730000},
			{"default-mac", 0x750000, 0x00200},
			{"pin", 0x750200, 0x00200},
			{"product-info", 0x750400, 0x0fc00},
			{"soft-version", 0x760000, 0x0b000},
			{"support-list", 0x76b000, 0x04000},
			{"profile", 0x770000, 0x04000},
			{"default-config", 0x774000, 0x0b000},
			{"user-config", 0x780000, 0x40000},
			{"partition-table", 0x7c0000, 0x10000},
			{"log", 0x7d0000, 0x20000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system",
	},

	/** Firmware layout for the TL-WR941HP v1 */
	{
		.id     = "TL-WR941HP-V1",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:TL-WR941HP,product_ver:1.0.0,special_id:00000000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"firmware", 0x20000, 0x730000},
			{"default-mac", 0x750000, 0x00200},
			{"pin", 0x750200, 0x00200},
			{"product-info", 0x750400, 0x0fc00},
			{"soft-version", 0x760000, 0x0b000},
			{"support-list", 0x76b000, 0x04000},
			{"profile", 0x770000, 0x04000},
			{"default-config", 0x774000, 0x0b000},
			{"user-config", 0x780000, 0x40000},
			{"partition-table", 0x7c0000, 0x10000},
			{"log", 0x7d0000, 0x20000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system",
	},

	/** Firmware layout for the TL-WR942N V1 */
	{
		.id     = "TLWR942NV1",
		.vendor = "",
		.support_list =
			"SupportList:\r\n"
			"{product_name:TL-WR942N,product_ver:1.0.0,special_id:00000000}\r\n"
			"{product_name:TL-WR942N,product_ver:1.0.0,special_id:52550000}\r\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"firmware", 0x20000, 0xe20000},
			{"default-mac", 0xe40000, 0x00200},
			{"pin", 0xe40200, 0x00200},
			{"product-info", 0xe40400, 0x0fc00},
			{"partition-table", 0xe50000, 0x10000},
			{"soft-version", 0xe60000, 0x10000},
			{"support-list", 0xe70000, 0x10000},
			{"profile", 0xe80000, 0x10000},
			{"default-config", 0xe90000, 0x10000},
			{"user-config", 0xea0000, 0x40000},
			{"qos-db", 0xee0000, 0x40000},
			{"certificate", 0xf20000, 0x10000},
			{"usb-config", 0xfb0000, 0x10000},
			{"log", 0xfc0000, 0x20000},
			{"radio-bk", 0xfe0000, 0x10000},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system",
	},

  /** Firmware layout for the RE200 v2 */
	{
		.id     = "RE200-V2",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:RE200,product_ver:2.0.0,special_id:00000000}\n"
			"{product_name:RE200,product_ver:2.0.0,special_id:41520000}\n"
			"{product_name:RE200,product_ver:2.0.0,special_id:41550000}\n"
			"{product_name:RE200,product_ver:2.0.0,special_id:42520000}\n"
			"{product_name:RE200,product_ver:2.0.0,special_id:43410000}\n"
			"{product_name:RE200,product_ver:2.0.0,special_id:45530000}\n"
			"{product_name:RE200,product_ver:2.0.0,special_id:45550000}\n"
			"{product_name:RE200,product_ver:2.0.0,special_id:49440000}\n"
			"{product_name:RE200,product_ver:2.0.0,special_id:4a500000}\n"
			"{product_name:RE200,product_ver:2.0.0,special_id:4b520000}\n"
			"{product_name:RE200,product_ver:2.0.0,special_id:52550000}\n"
			"{product_name:RE200,product_ver:2.0.0,special_id:54570000}\n"
			"{product_name:RE200,product_ver:2.0.0,special_id:55530000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"firmware", 0x20000, 0x7a0000},
			{"partition-table", 0x7c0000, 0x02000},
			{"default-mac", 0x7c2000, 0x00020},
			{"pin", 0x7c2100, 0x00020},
			{"product-info", 0x7c3100, 0x01000},
			{"soft-version", 0x7c4200, 0x01000},
			{"support-list", 0x7c5200, 0x01000},
			{"profile", 0x7c6200, 0x08000},
			{"config-info", 0x7ce200, 0x00400},
			{"user-config", 0x7d0000, 0x10000},
			{"default-config", 0x7e0000, 0x10000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

  /** Firmware layout for the RE200 v3 */
	{
		.id     = "RE200-V3",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:RE200,product_ver:3.0.0,special_id:00000000}\n"
			"{product_name:RE200,product_ver:3.0.0,special_id:41520000}\n"
			"{product_name:RE200,product_ver:3.0.0,special_id:41550000}\n"
			"{product_name:RE200,product_ver:3.0.0,special_id:42520000}\n"
			"{product_name:RE200,product_ver:3.0.0,special_id:43410000}\n"
			"{product_name:RE200,product_ver:3.0.0,special_id:45470000}\n"
			"{product_name:RE200,product_ver:3.0.0,special_id:45530000}\n"
			"{product_name:RE200,product_ver:3.0.0,special_id:45550000}\n"
			"{product_name:RE200,product_ver:3.0.0,special_id:49440000}\n"
			"{product_name:RE200,product_ver:3.0.0,special_id:4A500000}\n"
			"{product_name:RE200,product_ver:3.0.0,special_id:4B520000}\n"
			"{product_name:RE200,product_ver:3.0.0,special_id:52550000}\n"
			"{product_name:RE200,product_ver:3.0.0,special_id:54570000}\n"
			"{product_name:RE200,product_ver:3.0.0,special_id:55530000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"firmware", 0x20000, 0x7a0000},
			{"partition-table", 0x7c0000, 0x02000},
			{"default-mac", 0x7c2000, 0x00020},
			{"pin", 0x7c2100, 0x00020},
			{"product-info", 0x7c3100, 0x01000},
			{"soft-version", 0x7c4200, 0x01000},
			{"support-list", 0x7c5200, 0x01000},
			{"profile", 0x7c6200, 0x08000},
			{"config-info", 0x7ce200, 0x00400},
			{"user-config", 0x7d0000, 0x10000},
			{"default-config", 0x7e0000, 0x10000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

  /** Firmware layout for the RE200 v4 */
	{
		.id     = "RE200-V4",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:RE200,product_ver:4.0.0,special_id:00000000}\n"
			"{product_name:RE200,product_ver:4.0.0,special_id:45550000}\n"
			"{product_name:RE200,product_ver:4.0.0,special_id:4A500000}\n"
			"{product_name:RE200,product_ver:4.0.0,special_id:4B520000}\n"
			"{product_name:RE200,product_ver:4.0.0,special_id:43410000}\n"
			"{product_name:RE200,product_ver:4.0.0,special_id:41550000}\n"
			"{product_name:RE200,product_ver:4.0.0,special_id:42520000}\n"
			"{product_name:RE200,product_ver:4.0.0,special_id:55530000}\n"
			"{product_name:RE200,product_ver:4.0.0,special_id:41520000}\n"
			"{product_name:RE200,product_ver:4.0.0,special_id:52550000}\n"
			"{product_name:RE200,product_ver:4.0.0,special_id:54570000}\n"
			"{product_name:RE200,product_ver:4.0.0,special_id:45530000}\n"
			"{product_name:RE200,product_ver:4.0.0,special_id:49440000}\n"
			"{product_name:RE200,product_ver:4.0.0,special_id:45470000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_TEXT("soft_ver:1.1.0\n"),

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"firmware", 0x20000, 0x7a0000},
			{"partition-table", 0x7c0000, 0x02000},
			{"default-mac", 0x7c2000, 0x00020},
			{"pin", 0x7c2100, 0x00020},
			{"product-info", 0x7c3100, 0x01000},
			{"soft-version", 0x7c4200, 0x01000},
			{"support-list", 0x7c5200, 0x01000},
			{"profile", 0x7c6200, 0x08000},
			{"config-info", 0x7ce200, 0x00400},
			{"user-config", 0x7d0000, 0x10000},
			{"default-config", 0x7e0000, 0x10000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the RE220 v2 */
	{
		.id     = "RE220-V2",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:RE220,product_ver:2.0.0,special_id:00000000}\n"
			"{product_name:RE220,product_ver:2.0.0,special_id:41520000}\n"
			"{product_name:RE220,product_ver:2.0.0,special_id:41550000}\n"
			"{product_name:RE220,product_ver:2.0.0,special_id:42520000}\n"
			"{product_name:RE220,product_ver:2.0.0,special_id:43410000}\n"
			"{product_name:RE220,product_ver:2.0.0,special_id:45530000}\n"
			"{product_name:RE220,product_ver:2.0.0,special_id:45550000}\n"
			"{product_name:RE220,product_ver:2.0.0,special_id:49440000}\n"
			"{product_name:RE220,product_ver:2.0.0,special_id:4a500000}\n"
			"{product_name:RE220,product_ver:2.0.0,special_id:4b520000}\n"
			"{product_name:RE220,product_ver:2.0.0,special_id:52550000}\n"
			"{product_name:RE220,product_ver:2.0.0,special_id:54570000}\n"
			"{product_name:RE220,product_ver:2.0.0,special_id:55530000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"firmware", 0x20000, 0x7a0000},
			{"partition-table", 0x7c0000, 0x02000},
			{"default-mac", 0x7c2000, 0x00020},
			{"pin", 0x7c2100, 0x00020},
			{"product-info", 0x7c3100, 0x01000},
			{"soft-version", 0x7c4200, 0x01000},
			{"support-list", 0x7c5200, 0x01000},
			{"profile", 0x7c6200, 0x08000},
			{"config-info", 0x7ce200, 0x00400},
			{"user-config", 0x7d0000, 0x10000},
			{"default-config", 0x7e0000, 0x10000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

  /** Firmware layout for the RE305 v1 */
	{
		.id     = "RE305-V1",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:RE305,product_ver:1.0.0,special_id:45550000}\n"
			"{product_name:RE305,product_ver:1.0.0,special_id:55530000}\n"
			"{product_name:RE305,product_ver:1.0.0,special_id:4a500000}\n"
			"{product_name:RE305,product_ver:1.0.0,special_id:42520000}\n"
			"{product_name:RE305,product_ver:1.0.0,special_id:4b520000}\n"
			"{product_name:RE305,product_ver:1.0.0,special_id:41550000}\n"
			"{product_name:RE305,product_ver:1.0.0,special_id:43410000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"firmware", 0x20000, 0x5e0000},
			{"partition-table", 0x600000, 0x02000},
			{"default-mac", 0x610000, 0x00020},
			{"pin", 0x610100, 0x00020},
			{"product-info", 0x611100, 0x01000},
			{"soft-version", 0x620000, 0x01000},
			{"support-list", 0x621000, 0x01000},
			{"profile", 0x622000, 0x08000},
			{"user-config", 0x630000, 0x10000},
			{"default-config", 0x640000, 0x10000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the RE305 v3 */
	{
		.id     = "RE305-V3",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:RE305,product_ver:3.0.0,special_id:00000000}\n"
			"{product_name:RE305,product_ver:3.0.0,special_id:45550000}\n"
			"{product_name:RE305,product_ver:3.0.0,special_id:4A500000}\n"
			"{product_name:RE305,product_ver:3.0.0,special_id:4B520000}\n"
			"{product_name:RE305,product_ver:3.0.0,special_id:41550000}\n"
			"{product_name:RE305,product_ver:3.0.0,special_id:42520000}\n"
			"{product_name:RE305,product_ver:3.0.0,special_id:55530000}\n"
			"{product_name:RE305,product_ver:3.0.0,special_id:45530000}\n"
			"{product_name:RE305,product_ver:3.0.0,special_id:41530000}\n"
			"{product_name:RE305,product_ver:3.0.0,special_id:43410000}\n"
			"{product_name:RE305,product_ver:3.0.0,special_id:52550000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_TEXT("soft_ver:2.0.0\n"),

		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"firmware", 0x20000, 0x7a0000},
			{"partition-table", 0x7c0000, 0x02000},
			{"default-mac", 0x7c2000, 0x00020},
			{"pin", 0x7c2100, 0x00020},
			{"product-info", 0x7c3100, 0x01000},
			{"soft-version", 0x7c4200, 0x01000},
			{"support-list", 0x7c5200, 0x01000},
			{"profile", 0x7c6200, 0x08000},
			{"config-info", 0x7ce200, 0x00400},
			{"user-config", 0x7d0000, 0x10000},
			{"default-config", 0x7e0000, 0x10000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the RE350 v1 */
	{
		.id     = "RE350-V1",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:RE350,product_ver:1.0.0,special_id:45550000}\n"
			"{product_name:RE350,product_ver:1.0.0,special_id:00000000}\n"
			"{product_name:RE350,product_ver:1.0.0,special_id:41550000}\n"
			"{product_name:RE350,product_ver:1.0.0,special_id:55530000}\n"
			"{product_name:RE350,product_ver:1.0.0,special_id:43410000}\n"
			"{product_name:RE350,product_ver:1.0.0,special_id:4b520000}\n"
			"{product_name:RE350,product_ver:1.0.0,special_id:4a500000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		/** We're using a dynamic kernel/rootfs split here */
		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"firmware", 0x20000, 0x5e0000},
			{"partition-table", 0x600000, 0x02000},
			{"default-mac", 0x610000, 0x00020},
			{"pin", 0x610100, 0x00020},
			{"product-info", 0x611100, 0x01000},
			{"soft-version", 0x620000, 0x01000},
			{"support-list", 0x621000, 0x01000},
			{"profile", 0x622000, 0x08000},
			{"user-config", 0x630000, 0x10000},
			{"default-config", 0x640000, 0x10000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the RE350K v1 */
	{
		.id     = "RE350K-V1",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:RE350K,product_ver:1.0.0,special_id:00000000,product_region:US}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		/** We're using a dynamic kernel/rootfs split here */
		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"firmware", 0x20000, 0xd70000},
			{"partition-table", 0xd90000, 0x02000},
			{"default-mac", 0xda0000, 0x00020},
			{"pin", 0xda0100, 0x00020},
			{"product-info", 0xda1100, 0x01000},
			{"soft-version", 0xdb0000, 0x01000},
			{"support-list", 0xdb1000, 0x01000},
			{"profile", 0xdb2000, 0x08000},
			{"user-config", 0xdc0000, 0x10000},
			{"default-config", 0xdd0000, 0x10000},
			{"device-id", 0xde0000, 0x00108},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the RE355 */
	{
		.id     = "RE355",
		.vendor = "",
		.support_list =
			"SupportList:\r\n"
			"{product_name:RE355,product_ver:1.0.0,special_id:00000000}\r\n"
			"{product_name:RE355,product_ver:1.0.0,special_id:55530000}\r\n"
			"{product_name:RE355,product_ver:1.0.0,special_id:45550000}\r\n"
			"{product_name:RE355,product_ver:1.0.0,special_id:4A500000}\r\n"
			"{product_name:RE355,product_ver:1.0.0,special_id:43410000}\r\n"
			"{product_name:RE355,product_ver:1.0.0,special_id:41550000}\r\n"
			"{product_name:RE355,product_ver:1.0.0,special_id:4B520000}\r\n"
			"{product_name:RE355,product_ver:1.0.0,special_id:55534100}\r\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		/* We're using a dynamic kernel/rootfs split here */
		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"firmware", 0x20000, 0x5e0000},
			{"partition-table", 0x600000, 0x02000},
			{"default-mac", 0x610000, 0x00020},
			{"pin", 0x610100, 0x00020},
			{"product-info", 0x611100, 0x01000},
			{"soft-version", 0x620000, 0x01000},
			{"support-list", 0x621000, 0x01000},
			{"profile", 0x622000, 0x08000},
			{"user-config", 0x630000, 0x10000},
			{"default-config", 0x640000, 0x10000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the RE450 */
	{
		.id     = "RE450",
		.vendor = "",
		.support_list =
			"SupportList:\r\n"
			"{product_name:RE450,product_ver:1.0.0,special_id:00000000}\r\n"
			"{product_name:RE450,product_ver:1.0.0,special_id:55530000}\r\n"
			"{product_name:RE450,product_ver:1.0.0,special_id:45550000}\r\n"
			"{product_name:RE450,product_ver:1.0.0,special_id:4A500000}\r\n"
			"{product_name:RE450,product_ver:1.0.0,special_id:43410000}\r\n"
			"{product_name:RE450,product_ver:1.0.0,special_id:41550000}\r\n"
			"{product_name:RE450,product_ver:1.0.0,special_id:4B520000}\r\n"
			"{product_name:RE450,product_ver:1.0.0,special_id:55534100}\r\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		/** We're using a dynamic kernel/rootfs split here */
		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"firmware", 0x20000, 0x5e0000},
			{"partition-table", 0x600000, 0x02000},
			{"default-mac", 0x610000, 0x00020},
			{"pin", 0x610100, 0x00020},
			{"product-info", 0x611100, 0x01000},
			{"soft-version", 0x620000, 0x01000},
			{"support-list", 0x621000, 0x01000},
			{"profile", 0x622000, 0x08000},
			{"user-config", 0x630000, 0x10000},
			{"default-config", 0x640000, 0x10000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the RE450 v2 */
	{
		.id     = "RE450-V2",
		.vendor = "",
		.support_list =
			"SupportList:\r\n"
			"{product_name:RE450,product_ver:2.0.0,special_id:00000000}\r\n"
			"{product_name:RE450,product_ver:2.0.0,special_id:55530000}\r\n"
			"{product_name:RE450,product_ver:2.0.0,special_id:45550000}\r\n"
			"{product_name:RE450,product_ver:2.0.0,special_id:4A500000}\r\n"
			"{product_name:RE450,product_ver:2.0.0,special_id:43410000}\r\n"
			"{product_name:RE450,product_ver:2.0.0,special_id:41550000}\r\n"
			"{product_name:RE450,product_ver:2.0.0,special_id:41530000}\r\n"
			"{product_name:RE450,product_ver:2.0.0,special_id:4B520000}\r\n"
			"{product_name:RE450,product_ver:2.0.0,special_id:42520000}\r\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		/* We're using a dynamic kernel/rootfs split here */
		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"firmware", 0x20000, 0x5e0000},
			{"partition-table", 0x600000, 0x02000},
			{"default-mac", 0x610000, 0x00020},
			{"pin", 0x610100, 0x00020},
			{"product-info", 0x611100, 0x01000},
			{"soft-version", 0x620000, 0x01000},
			{"support-list", 0x621000, 0x01000},
			{"profile", 0x622000, 0x08000},
			{"user-config", 0x630000, 0x10000},
			{"default-config", 0x640000, 0x10000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the RE450 v3 */
	{
		.id     = "RE450-V3",
		.vendor = "",
		.support_list =
			"SupportList:\r\n"
			"{product_name:RE450,product_ver:3.0.0,special_id:00000000}\r\n"
			"{product_name:RE450,product_ver:3.0.0,special_id:55530000}\r\n"
			"{product_name:RE450,product_ver:3.0.0,special_id:45550000}\r\n"
			"{product_name:RE450,product_ver:3.0.0,special_id:4A500000}\r\n"
			"{product_name:RE450,product_ver:3.0.0,special_id:43410000}\r\n"
			"{product_name:RE450,product_ver:3.0.0,special_id:41550000}\r\n"
			"{product_name:RE450,product_ver:3.0.0,special_id:41530000}\r\n"
			"{product_name:RE450,product_ver:3.0.0,special_id:4B520000}\r\n"
			"{product_name:RE450,product_ver:3.0.0,special_id:42520000}\r\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		/* We're using a dynamic kernel/rootfs split here */
		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"default-mac", 0x20000, 0x00020},
			{"pin", 0x20020, 0x00020},
			{"product-info", 0x21000, 0x01000},
			{"partition-table", 0x22000, 0x02000},
			{"soft-version", 0x24000, 0x01000},
			{"support-list", 0x25000, 0x01000},
			{"profile", 0x26000, 0x08000},
			{"user-config", 0x2e000, 0x10000},
			{"default-config", 0x3e000, 0x10000},
			{"config-info", 0x4e000, 0x00400},
			{"firmware", 0x50000, 0x7a0000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the RE455 v1 */
	{
		.id     = "RE455-V1",
		.vendor = "",
		.support_list =
			"SupportList:\r\n"
			"{product_name:RE455,product_ver:1.0.0,special_id:00000000}\r\n"
			"{product_name:RE455,product_ver:1.0.0,special_id:55530000}\r\n"
			"{product_name:RE455,product_ver:1.0.0,special_id:45550000}\r\n"
			"{product_name:RE455,product_ver:1.0.0,special_id:4A500000}\r\n"
			"{product_name:RE455,product_ver:1.0.0,special_id:43410000}\r\n"
			"{product_name:RE455,product_ver:1.0.0,special_id:41550000}\r\n"
			"{product_name:RE455,product_ver:1.0.0,special_id:41530000}\r\n"
			"{product_name:RE455,product_ver:1.0.0,special_id:4B520000}\r\n"
			"{product_name:RE455,product_ver:1.0.0,special_id:42520000}\r\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		/* We're using a dynamic kernel/rootfs split here */
		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"default-mac", 0x20000, 0x00020},
			{"pin", 0x20020, 0x00020},
			{"product-info", 0x21000, 0x01000},
			{"partition-table", 0x22000, 0x02000},
			{"soft-version", 0x24000, 0x01000},
			{"support-list", 0x25000, 0x01000},
			{"profile", 0x26000, 0x08000},
			{"user-config", 0x2e000, 0x10000},
			{"default-config", 0x3e000, 0x10000},
			{"config-info", 0x4e000, 0x00400},
			{"firmware", 0x50000, 0x7a0000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the RE500 */
	{
		.id     = "RE500-V1",
		.vendor = "",
		.support_list =
			"SupportList:\r\n"
			"{product_name:RE500,product_ver:1.0.0,special_id:00000000}\r\n"
			"{product_name:RE500,product_ver:1.0.0,special_id:55530000}\r\n"
			"{product_name:RE500,product_ver:1.0.0,special_id:45550000}\r\n"
			"{product_name:RE500,product_ver:1.0.0,special_id:4A500000}\r\n"
			"{product_name:RE500,product_ver:1.0.0,special_id:43410000}\r\n"
			"{product_name:RE500,product_ver:1.0.0,special_id:41550000}\r\n"
			"{product_name:RE500,product_ver:1.0.0,special_id:41530000}\r\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		/* We're using a dynamic kernel/rootfs split here */
		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"firmware", 0x20000, 0xde0000},
			{"partition-table", 0xe00000, 0x02000},
			{"default-mac", 0xe10000, 0x00020},
			{"pin", 0xe10100, 0x00020},
			{"product-info", 0xe11100, 0x01000},
			{"soft-version", 0xe20000, 0x01000},
			{"support-list", 0xe21000, 0x01000},
			{"profile", 0xe22000, 0x08000},
			{"user-config", 0xe30000, 0x10000},
			{"default-config", 0xe40000, 0x10000},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the RE650 */
	{
		.id     = "RE650-V1",
		.vendor = "",
		.support_list =
			"SupportList:\r\n"
			"{product_name:RE650,product_ver:1.0.0,special_id:00000000}\r\n"
			"{product_name:RE650,product_ver:1.0.0,special_id:55530000}\r\n"
			"{product_name:RE650,product_ver:1.0.0,special_id:45550000}\r\n"
			"{product_name:RE650,product_ver:1.0.0,special_id:4A500000}\r\n"
			"{product_name:RE650,product_ver:1.0.0,special_id:43410000}\r\n"
			"{product_name:RE650,product_ver:1.0.0,special_id:41550000}\r\n"
			"{product_name:RE650,product_ver:1.0.0,special_id:41530000}\r\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		/* We're using a dynamic kernel/rootfs split here */
		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"firmware", 0x20000, 0xde0000},
			{"partition-table", 0xe00000, 0x02000},
			{"default-mac", 0xe10000, 0x00020},
			{"pin", 0xe10100, 0x00020},
			{"product-info", 0xe11100, 0x01000},
			{"soft-version", 0xe20000, 0x01000},
			{"support-list", 0xe21000, 0x01000},
			{"profile", 0xe22000, 0x08000},
			{"user-config", 0xe30000, 0x10000},
			{"default-config", 0xe40000, 0x10000},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},
	/** Firmware layout for the RE650 V2 (8MB Flash)*/
	{
		.id     = "RE650-V2",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:RE650,product_ver:2.0.0,special_id:00000000}\n"
			"{product_name:RE650,product_ver:2.0.0,special_id:45550000}\n"
			"{product_name:RE650,product_ver:2.0.0,special_id:4A500000}\n"
			"{product_name:RE650,product_ver:2.0.0,special_id:41550000}\n"
			"{product_name:RE650,product_ver:2.0.0,special_id:43410000}\n"
			"{product_name:RE650,product_ver:2.0.0,special_id:41530000}\n"
			"{product_name:RE650,product_ver:2.0.0,special_id:55530000}\n",
		.part_trail = 0x00,
		/* For RE650 v2, soft ver is required, otherwise OEM install doesn't work */
		.soft_ver = SOFT_VER_TEXT("soft_ver:2.0.0\n"),

		/* We're using a dynamic kernel/rootfs split here */
		.partitions = {
			{"fs-uboot", 0x00000, 0x20000},
			{"firmware", 0x20000, 0x7a0000},
			{"partition-table", 0x7c0000, 0x02000},
			{"default-mac", 0x7c2000, 0x00020},
			{"pin", 0x7c2100, 0x00020},
			{"product-info", 0x7c3100, 0x01000},
			{"soft-version", 0x7c4200, 0x01000},
			{"support-list", 0x7c5200, 0x01000},
			{"profile", 0x7c6200, 0x08000},
			{"config-info", 0x7ce200, 0x00400},
			{"user-config", 0x7d0000, 0x10000},
			{"default-config", 0x7e0000, 0x10000},
			{"radio", 0x7f0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	/** Firmware layout for the Mercusys MR70X */
	{
		.id     = "MR70X",
		.vendor = "",
		.support_list =
			"SupportList:\n"
			"{product_name:MR70X,product_ver:1.0.0,special_id:45550000}\n"
			"{product_name:MR70X,product_ver:1.0.0,special_id:4A500000}\n"
			"{product_name:MR70X,product_ver:1.0.0,special_id:55530000}\n",
		.part_trail = 0x00,
		.soft_ver = SOFT_VER_DEFAULT,

		.partitions = {
			{"fs-uboot", 0x00000, 0x40000},
			{"firmware", 0x40000, 0xf60000},
			{"default-mac", 0xfa0000, 0x00200},
			{"pin", 0xfa0200, 0x00100},
			{"device-id", 0xfa0300, 0x00100},
			{"product-info", 0xfa0400, 0x0fc00},
			{"default-config", 0xfb0000, 0x08000},
			{"ap-def-config", 0xfb8000, 0x08000},
			{"user-config", 0xfc0000, 0x0a000},
			{"ag-config", 0xfca000, 0x04000},
			{"certificate", 0xfce000, 0x02000},
			{"ap-config", 0xfd0000, 0x06000},
			{"router-config", 0xfd6000, 0x06000},
			{"favicon", 0xfdc000, 0x02000},
			{"logo", 0xfde000, 0x02000},
			{"partition-table", 0xfe0000, 0x00800},
			{"soft-version", 0xfe0800, 0x00100},
			{"support-list", 0xfe0900, 0x00200},
			{"profile", 0xfe0b00, 0x03000},
			{"extra-para", 0xfe3b00, 0x00100},
			{"radio", 0xff0000, 0x10000},
			{NULL, 0, 0}
		},

		.first_sysupgrade_partition = "os-image",
		.last_sysupgrade_partition = "file-system"
	},

	{}
};

#define error(_ret, _errno, _str, ...)				\
	do {							\
		fprintf(stderr, _str ": %s\n", ## __VA_ARGS__,	\
			strerror(_errno));			\
		if (_ret)					\
			exit(_ret);				\
	} while (0)


/** Stores a uint32 as big endian */
static inline void put32(uint8_t *buf, uint32_t val) {
	buf[0] = val >> 24;
	buf[1] = val >> 16;
	buf[2] = val >> 8;
	buf[3] = val;
}

static inline bool meta_partition_should_pad(enum partition_trail_value pv)
{
	return (pv >= 0) && (pv <= PART_TRAIL_MAX);
}

/** Allocate a padded meta partition with a correctly initialised header
 * If the `data` pointer is NULL, then the required space is only allocated,
 * otherwise `data_len` bytes will be copied from `data` into the partition
 * entry. */
static struct image_partition_entry init_meta_partition_entry(
	const char *name, const void *data, uint32_t data_len,
	enum partition_trail_value pad_value)
{
	uint32_t total_len = sizeof(struct meta_header) + data_len;
	if (meta_partition_should_pad(pad_value))
		total_len += 1;

	struct image_partition_entry entry = {
		.name = name,
		.size = total_len,
		.data = malloc(total_len)
	};
	if (!entry.data)
		error(1, errno, "failed to allocate meta partition entry");

	struct meta_header *header = (struct meta_header *)entry.data;
	header->length = htonl(data_len);
	header->zero = 0;

	if (data)
		memcpy(entry.data+sizeof(*header), data, data_len);

	if (meta_partition_should_pad(pad_value))
		entry.data[total_len - 1] = (uint8_t) pad_value;

	return entry;
}

/** Allocates a new image partition */
static struct image_partition_entry alloc_image_partition(const char *name, size_t len) {
	struct image_partition_entry entry = {name, len, malloc(len)};
	if (!entry.data)
		error(1, errno, "malloc");

	return entry;
}

/** Sets up default partition names whenever custom names aren't specified */
static void set_partition_names(struct device_info *info)
{
	if (!info->partition_names.partition_table)
		info->partition_names.partition_table = "partition-table";
	if (!info->partition_names.soft_ver)
		info->partition_names.soft_ver = "soft-version";
	if (!info->partition_names.os_image)
		info->partition_names.os_image = "os-image";
	if (!info->partition_names.support_list)
		info->partition_names.support_list = "support-list";
	if (!info->partition_names.file_system)
		info->partition_names.file_system = "file-system";
	if (!info->partition_names.extra_para)
		info->partition_names.extra_para = "extra-para";
}

/** Frees an image partition */
static void free_image_partition(struct image_partition_entry *entry)
{
	void *data = entry->data;

	entry->name = NULL;
	entry->size = 0;
	entry->data = NULL;

	free(data);
}

static time_t source_date_epoch = -1;
static void set_source_date_epoch() {
	char *env = getenv("SOURCE_DATE_EPOCH");
	char *endptr = env;
	errno = 0;
	if (env && *env) {
		source_date_epoch = strtoull(env, &endptr, 10);
		if (errno || (endptr && *endptr != '\0')) {
			fprintf(stderr, "Invalid SOURCE_DATE_EPOCH");
			exit(1);
		}
	}
}

/** Generates the partition-table partition */
static struct image_partition_entry make_partition_table(const struct device_info *p)
{
	struct image_partition_entry entry = alloc_image_partition(p->partition_names.partition_table, SAFELOADER_PAYLOAD_TABLE_SIZE);

	char *s = (char *)entry.data, *end = (char *)(s+entry.size);

	*(s++) = 0x00;
	*(s++) = 0x04;
	*(s++) = 0x00;
	*(s++) = 0x00;

	size_t i;
	for (i = 0; p->partitions[i].name; i++) {
		size_t len = end-s;
		size_t w = snprintf(s, len, "partition %s base 0x%05x size 0x%05x\n",
			p->partitions[i].name, p->partitions[i].base, p->partitions[i].size);

		if (w > len-1)
			error(1, 0, "flash partition table overflow?");

		s += w;
	}

	s++;

	memset(s, 0xff, end-s);

	return entry;
}


/** Generates a binary-coded decimal representation of an integer in the range [0, 99] */
static inline uint8_t bcd(uint8_t v) {
	return 0x10 * (v/10) + v%10;
}


/** Generates the soft-version partition */
static struct image_partition_entry make_soft_version(const struct device_info *info, uint32_t rev)
{
	/** If an info string is provided, use this instead of
	 * the structured data, and include the null-termination */
	if (info->soft_ver.type == SOFT_VER_TYPE_TEXT) {
		uint32_t len = strlen(info->soft_ver.text) + 1;
		return init_meta_partition_entry(info->partition_names.soft_ver,
			info->soft_ver.text, len, info->part_trail);
	}

	time_t t;

	if (source_date_epoch != -1)
		t = source_date_epoch;
	else if (time(&t) == (time_t)(-1))
		error(1, errno, "time");

	struct tm *tm = gmtime(&t);

	struct soft_version s = {
		.pad1 = 0xff,

		.version_major = info->soft_ver.num[0],
		.version_minor = info->soft_ver.num[1],
		.version_patch = info->soft_ver.num[2],

		.year_hi = bcd((1900+tm->tm_year)/100),
		.year_lo = bcd(tm->tm_year%100),
		.month = bcd(tm->tm_mon+1),
		.day = bcd(tm->tm_mday),
		.rev = htonl(rev),

		.compat_level = htonl(info->soft_ver_compat_level)
	};

	if (info->soft_ver_compat_level == 0)
		return init_meta_partition_entry(info->partition_names.soft_ver, &s,
			(uint8_t *)(&s.compat_level) - (uint8_t *)(&s),
			info->part_trail);
	else
		return init_meta_partition_entry(info->partition_names.soft_ver, &s,
			sizeof(s), info->part_trail);
}

/** Generates the support-list partition */
static struct image_partition_entry make_support_list(
	const struct device_info *info)
{
	uint32_t len = strlen(info->support_list);
	return init_meta_partition_entry(info->partition_names.support_list, info->support_list,
		len, info->part_trail);
}

/** Partition with extra-para data */
static struct image_partition_entry make_extra_para(
	const struct device_info *info, const uint8_t *extra_para, size_t len)
{
	return init_meta_partition_entry(info->partition_names.extra_para, extra_para, len,
		info->part_trail);
}

/** Creates a new image partition with an arbitrary name from a file */
static struct image_partition_entry read_file(const char *part_name, const char *filename, bool add_jffs2_eof, struct flash_partition_entry *file_system_partition) {
	struct stat statbuf;

	if (stat(filename, &statbuf) < 0)
		error(1, errno, "unable to stat file `%s'", filename);

	size_t len = statbuf.st_size;

	if (add_jffs2_eof) {
		if (file_system_partition)
			len = ALIGN(len + file_system_partition->base, 0x10000) + sizeof(jffs2_eof_mark) - file_system_partition->base;
		else
			len = ALIGN(len, 0x10000) + sizeof(jffs2_eof_mark);
	}

	struct image_partition_entry entry = alloc_image_partition(part_name, len);

	FILE *file = fopen(filename, "rb");
	if (!file)
		error(1, errno, "unable to open file `%s'", filename);

	if (fread(entry.data, statbuf.st_size, 1, file) != 1)
		error(1, errno, "unable to read file `%s'", filename);

	if (add_jffs2_eof) {
		uint8_t *eof = entry.data + statbuf.st_size, *end = entry.data+entry.size;

		memset(eof, 0xff, end - eof - sizeof(jffs2_eof_mark));
		memcpy(end - sizeof(jffs2_eof_mark), jffs2_eof_mark, sizeof(jffs2_eof_mark));
	}

	fclose(file);

	return entry;
}

/**
   Copies a list of image partitions into an image buffer and generates the image partition table while doing so

   Example image partition table:

     fwup-ptn partition-table base 0x00800 size 0x00800
     fwup-ptn os-image base 0x01000 size 0x113b45
     fwup-ptn file-system base 0x114b45 size 0x1d0004
     fwup-ptn support-list base 0x2e4b49 size 0x000d1

   Each line of the partition table is terminated with the bytes 09 0d 0a ("\t\r\n"),
   the end of the partition table is marked with a zero byte.

   The firmware image must contain at least the partition-table and support-list partitions
   to be accepted. There aren't any alignment constraints for the image partitions.

   The partition-table partition contains the actual flash layout; partitions
   from the image partition table are mapped to the corresponding flash partitions during
   the firmware upgrade. The support-list partition contains a list of devices supported by
   the firmware image.

   The base offsets in the firmware partition table are relative to the end
   of the vendor information block, so the partition-table partition will
   actually start at offset 0x1814 of the image.

   I think partition-table must be the first partition in the firmware image.
*/
static void put_partitions(uint8_t *buffer, const struct flash_partition_entry *flash_parts, const struct image_partition_entry *parts) {
	size_t i, j;
	char *image_pt = (char *)buffer, *end = image_pt + SAFELOADER_PAYLOAD_TABLE_SIZE;

	size_t base = SAFELOADER_PAYLOAD_TABLE_SIZE;
	for (i = 0; parts[i].name; i++) {
		for (j = 0; flash_parts[j].name; j++) {
			if (!strcmp(flash_parts[j].name, parts[i].name)) {
				if (parts[i].size > flash_parts[j].size)
					error(1, 0, "%s partition too big (more than %u bytes)", flash_parts[j].name, (unsigned)flash_parts[j].size);
				break;
			}
		}

		assert(flash_parts[j].name);

		memcpy(buffer + base, parts[i].data, parts[i].size);

		size_t len = end-image_pt;
		size_t w = snprintf(image_pt, len, "fwup-ptn %s base 0x%05x size 0x%05x\t\r\n", parts[i].name, (unsigned)base, (unsigned)parts[i].size);

		if (w > len-1)
			error(1, 0, "image partition table overflow?");

		image_pt += w;

		base += parts[i].size;
	}
}

/** Generates and writes the image MD5 checksum */
static void put_md5(uint8_t *md5, uint8_t *buffer, unsigned int len) {
	MD5_CTX ctx;

	MD5_Init(&ctx);
	MD5_Update(&ctx, md5_salt, (unsigned int)sizeof(md5_salt));
	MD5_Update(&ctx, buffer, len);
	MD5_Final(md5, &ctx);
}


/**
   Generates the firmware image in factory format

   Image format:

     Bytes (hex)  Usage
     -----------  -----
     0000-0003    Image size (4 bytes, big endian)
     0004-0013    MD5 hash (hash of a 16 byte salt and the image data starting with byte 0x14)
     0014-0017    Vendor information length (without padding) (4 bytes, big endian)
     0018-1013    Vendor information (4092 bytes, padded with 0xff; there seem to be older
                  (VxWorks-based) TP-LINK devices which use a smaller vendor information block)
     1014-1813    Image partition table (2048 bytes, padded with 0xff)
     1814-xxxx    Firmware partitions
*/
static void * generate_factory_image(struct device_info *info, const struct image_partition_entry *parts, size_t *len) {
	*len = SAFELOADER_PAYLOAD_OFFSET + SAFELOADER_PAYLOAD_TABLE_SIZE;

	size_t i;
	for (i = 0; parts[i].name; i++)
		*len += parts[i].size;

	uint8_t *image = malloc(*len);
	if (!image)
		error(1, errno, "malloc");

	memset(image, 0xff, *len);
	put32(image, *len);

	if (info->vendor) {
		size_t vendor_len = strlen(info->vendor);
		put32(image + SAFELOADER_PREAMBLE_SIZE, vendor_len);
		memcpy(image + SAFELOADER_PREAMBLE_SIZE + 0x4, info->vendor, vendor_len);
	}

	put_partitions(image + SAFELOADER_PAYLOAD_OFFSET, info->partitions, parts);
	put_md5(image + 0x04, image + SAFELOADER_PREAMBLE_SIZE, *len - SAFELOADER_PREAMBLE_SIZE);

	return image;
}

/**
   Generates the firmware image in sysupgrade format

   This makes some assumptions about the provided flash and image partition tables and
   should be generalized when TP-LINK starts building its safeloader into hardware with
   different flash layouts.
*/
static void * generate_sysupgrade_image(struct device_info *info, const struct image_partition_entry *image_parts, size_t *len) {
	size_t i, j;
	size_t flash_first_partition_index = 0;
	size_t flash_last_partition_index = 0;
	const struct flash_partition_entry *flash_first_partition = NULL;
	const struct flash_partition_entry *flash_last_partition = NULL;
	const struct image_partition_entry *image_last_partition = NULL;

	/** Find first and last partitions */
	for (i = 0; info->partitions[i].name; i++) {
		if (!strcmp(info->partitions[i].name, info->first_sysupgrade_partition)) {
			flash_first_partition = &info->partitions[i];
			flash_first_partition_index = i;
		} else if (!strcmp(info->partitions[i].name, info->last_sysupgrade_partition)) {
			flash_last_partition = &info->partitions[i];
			flash_last_partition_index = i;
		}
	}

	assert(flash_first_partition && flash_last_partition);
	assert(flash_first_partition_index < flash_last_partition_index);

	/** Find last partition from image to calculate needed size */
	for (i = 0; image_parts[i].name; i++) {
		if (!strcmp(image_parts[i].name, info->last_sysupgrade_partition)) {
			image_last_partition = &image_parts[i];
			break;
		}
	}

	assert(image_last_partition);

	*len = flash_last_partition->base - flash_first_partition->base + image_last_partition->size;

	uint8_t *image = malloc(*len);
	if (!image)
		error(1, errno, "malloc");

	memset(image, 0xff, *len);

	for (i = flash_first_partition_index; i <= flash_last_partition_index; i++) {
		for (j = 0; image_parts[j].name; j++) {
			if (!strcmp(info->partitions[i].name, image_parts[j].name)) {
				if (image_parts[j].size > info->partitions[i].size)
					error(1, 0, "%s partition too big (more than %u bytes)", info->partitions[i].name, (unsigned)info->partitions[i].size);
				memcpy(image + info->partitions[i].base - flash_first_partition->base, image_parts[j].data, image_parts[j].size);
				break;
			}

			assert(image_parts[j].name);
		}
	}

	return image;
}

/** Generates an image according to a given layout and writes it to a file */
static void build_image(const char *output,
		const char *kernel_image,
		const char *rootfs_image,
		uint32_t rev,
		bool add_jffs2_eof,
		bool sysupgrade,
		struct device_info *info) {

	size_t i;

	struct image_partition_entry parts[7] = {};

	struct flash_partition_entry *firmware_partition = NULL;
	struct flash_partition_entry *os_image_partition = NULL;
	struct flash_partition_entry *file_system_partition = NULL;
	size_t firmware_partition_index = 0;

	set_partition_names(info);

	for (i = 0; info->partitions[i].name; i++) {
		if (!strcmp(info->partitions[i].name, "firmware"))
		{
			firmware_partition = &info->partitions[i];
			firmware_partition_index = i;
		}
	}

	if (firmware_partition)
	{
		os_image_partition = &info->partitions[firmware_partition_index];
		file_system_partition = &info->partitions[firmware_partition_index + 1];

		struct stat kernel;
		if (stat(kernel_image, &kernel) < 0)
			error(1, errno, "unable to stat file `%s'", kernel_image);

		if (kernel.st_size > firmware_partition->size)
			error(1, 0, "kernel overflowed firmware partition\n");

		for (i = MAX_PARTITIONS-1; i >= firmware_partition_index + 1; i--)
			info->partitions[i+1] = info->partitions[i];

		file_system_partition->name = info->partition_names.file_system;

		file_system_partition->base = firmware_partition->base + kernel.st_size;

		/* Align partition start to erase blocks for factory images only */
		if (!sysupgrade)
			file_system_partition->base = ALIGN(firmware_partition->base + kernel.st_size, 0x10000);

		file_system_partition->size = firmware_partition->size - file_system_partition->base;

		os_image_partition->name = info->partition_names.os_image;

		os_image_partition->size = kernel.st_size;
	}

	parts[0] = make_partition_table(info);
	parts[1] = make_soft_version(info, rev);
	parts[2] = make_support_list(info);
	parts[3] = read_file(info->partition_names.os_image, kernel_image, false, NULL);
	parts[4] = read_file(info->partition_names.file_system, rootfs_image, add_jffs2_eof, file_system_partition);


	/* Some devices need the extra-para partition to accept the firmware */
	if (strcasecmp(info->id, "ARCHER-A6-V3") == 0 ||
	    strcasecmp(info->id, "ARCHER-A7-V5") == 0 ||
	    strcasecmp(info->id, "ARCHER-A9-V6") == 0 ||
	    strcasecmp(info->id, "ARCHER-AX23-V1") == 0 ||
	    strcasecmp(info->id, "ARCHER-C2-V3") == 0 ||
	    strcasecmp(info->id, "ARCHER-C7-V4") == 0 ||
	    strcasecmp(info->id, "ARCHER-C7-V5") == 0 ||
	    strcasecmp(info->id, "ARCHER-C25-V1") == 0 ||
	    strcasecmp(info->id, "ARCHER-C59-V2") == 0 ||
	    strcasecmp(info->id, "ARCHER-C60-V2") == 0 ||
	    strcasecmp(info->id, "ARCHER-C60-V3") == 0 ||
	    strcasecmp(info->id, "ARCHER-C6U-V1") == 0 ||
	    strcasecmp(info->id, "ARCHER-C6-V3") == 0 ||
	    strcasecmp(info->id, "DECO-M4R-V4") == 0 ||
	    strcasecmp(info->id, "MR70X") == 0 ||
	    strcasecmp(info->id, "TLWR1043NV5") == 0) {
		const uint8_t extra_para[2] = {0x01, 0x00};
		parts[5] = make_extra_para(info, extra_para,
			sizeof(extra_para));
	} else if (strcasecmp(info->id, "ARCHER-C6-V2") == 0 ||
		   strcasecmp(info->id, "TL-WA1201-V2") == 0) {
		const uint8_t extra_para[2] = {0x00, 0x01};
		parts[5] = make_extra_para(info, extra_para,
			sizeof(extra_para));
	} else if (strcasecmp(info->id, "ARCHER-C6-V2-US") == 0 ||
		   strcasecmp(info->id, "EAP245-V3") == 0) {
		const uint8_t extra_para[2] = {0x01, 0x01};
		parts[5] = make_extra_para(info, extra_para,
			sizeof(extra_para));
	}

	size_t len;
	void *image;
	if (sysupgrade)
		image = generate_sysupgrade_image(info, parts, &len);
	else
		image = generate_factory_image(info, parts, &len);

	FILE *file = fopen(output, "wb");
	if (!file)
		error(1, errno, "unable to open output file");

	if (fwrite(image, len, 1, file) != 1)
		error(1, 0, "unable to write output file");

	fclose(file);

	free(image);

	for (i = 0; parts[i].name; i++)
		free_image_partition(&parts[i]);
}

/** Usage output */
static void usage(const char *argv0) {
	fprintf(stderr,
		"Usage: %s [OPTIONS...]\n"
		"\n"
		"Options:\n"
		"  -h              show this help\n"
		"\n"
		"Info about an image:\n"
		"  -i <file>       input file to read from\n"
		"Create a new image:\n"
		"  -B <board>      create image for the board specified with <board>\n"
		"  -k <file>       read kernel image from the file <file>\n"
		"  -r <file>       read rootfs image from the file <file>\n"
		"  -o <file>       write output to the file <file>\n"
		"  -V <rev>        sets the revision number to <rev>\n"
		"  -j              add jffs2 end-of-filesystem markers\n"
		"  -S              create sysupgrade instead of factory image\n"
		"Extract an old image:\n"
		"  -x <file>       extract all oem firmware partition\n"
		"  -d <dir>        destination to extract the firmware partition\n"
		"  -z <file>       convert an oem firmware into a sysupgade file. Use -o for output file\n",
		argv0
	);
};


static struct device_info *find_board(const char *id)
{
	struct device_info *board = NULL;

	for (board = boards; board->id != NULL; board++)
		if (strcasecmp(id, board->id) == 0)
			return board;

	return NULL;
}

static int add_flash_partition(
		struct flash_partition_entry *part_list,
		size_t max_entries,
		const char *name,
		unsigned long base,
		unsigned long size)
{
	size_t ptr;
	/* check if the list has a free entry */
	for (ptr = 0; ptr < max_entries; ptr++, part_list++) {
		if (part_list->name == NULL &&
				part_list->base == 0 &&
				part_list->size == 0)
			break;
	}

	if (ptr == max_entries) {
		error(1, 0, "No free flash part entry available.");
	}

	part_list->name = calloc(1, strlen(name) + 1);
	if (!part_list->name) {
		error(1, 0, "Unable to allocate memory");
	}

	memcpy((char *)part_list->name, name, strlen(name));
	part_list->base = base;
	part_list->size = size;

	return 0;
}

/** read the partition table into struct flash_partition_entry */
enum PARTITION_TABLE_TYPE {
	PARTITION_TABLE_FWUP,
	PARTITION_TABLE_FLASH,
};

static int read_partition_table(
		FILE *file, long offset,
		struct flash_partition_entry *entries, size_t max_entries,
		int type)
{
	char buf[SAFELOADER_PAYLOAD_TABLE_SIZE];
	char *ptr, *end;
	const char *parthdr = NULL;
	const char *fwuphdr = "fwup-ptn";
	const char *flashhdr = "partition";

	/* TODO: search for the partition table */

	switch(type) {
	case PARTITION_TABLE_FWUP:
		parthdr = fwuphdr;
		break;
	case PARTITION_TABLE_FLASH:
		parthdr = flashhdr;
		break;
	default:
		error(1, 0, "Invalid partition table");
	}

	if (fseek(file, offset, SEEK_SET) < 0)
		error(1, errno, "Can not seek in the firmware");

	if (fread(buf, sizeof(buf), 1, file) != 1)
		error(1, errno, "Can not read fwup-ptn from the firmware");

	buf[sizeof(buf) - 1] = '\0';

	/* look for the partition header */
	if (memcmp(buf, parthdr, strlen(parthdr)) != 0) {
		fprintf(stderr, "DEBUG: can not find fwuphdr\n");
		return 1;
	}

	ptr = buf;
	end = buf + sizeof(buf);
	while ((ptr + strlen(parthdr)) < end &&
			memcmp(ptr, parthdr, strlen(parthdr)) == 0) {
		char *end_part;
		char *end_element;

		char name[32] = { 0 };
		int name_len = 0;
		unsigned long base = 0;
		unsigned long size = 0;

		end_part = memchr(ptr, '\n', (end - ptr));
		if (end_part == NULL) {
			/* in theory this should never happen, because a partition always ends with 0x09, 0x0D, 0x0A */
			break;
		}

		for (int i = 0; i <= 4; i++) {
			if (end_part <= ptr)
				break;

			end_element = memchr(ptr, 0x20, (end_part - ptr));
			if (end_element == NULL) {
				error(1, errno, "Ignoring the rest of the partition entries.");
				break;
			}

			switch (i) {
				/* partition header */
				case 0:
					ptr = end_element + 1;
					continue;
				/* name */
				case 1:
					name_len = (end_element - ptr) > 31 ? 31 : (end_element - ptr);
					strncpy(name, ptr, name_len);
					name[name_len] = '\0';
					ptr = end_element + 1;
					continue;

				/* string "base" */
				case 2:
					ptr = end_element + 1;
					continue;

				/* actual base */
				case 3:
					base = strtoul(ptr, NULL, 16);
					ptr = end_element + 1;
					continue;

				/* string "size" */
				case 4:
					ptr = end_element + 1;
					/* actual size. The last element doesn't have a sepeartor */
					size = strtoul(ptr, NULL, 16);
					/* the part ends with 0x09, 0x0d, 0x0a */
					ptr = end_part + 1;
					add_flash_partition(entries, max_entries, name, base, size);
					continue;
			}
		}
	}

	return 0;
}

static void safeloader_read_partition(FILE *input_file, size_t payload_offset,
				      struct flash_partition_entry *entry,
				      struct image_partition_entry *part)
{
	size_t part_size = entry->size;
	void *part_data = malloc(part_size);

	if (fseek(input_file, payload_offset, SEEK_SET))
		error(1, errno, "Failed to seek to partition data");

	if (!part_data)
		error(1, ENOMEM, "Failed to allocate partition data");

	if (fread(part_data, 1, part_size, input_file) < part_size)
		error(1, errno, "Failed to read partition data");

	part->data = part_data;
	part->size = part_size;
	part->name = entry->name;
}

static void safeloader_parse_image(FILE *input_file, struct safeloader_image_info *image)
{
	static const char *HEADER_ID_CLOUD = "fw-type:Cloud";
	static const char *HEADER_ID_QNEW = "?NEW";

	char buf[64];

	if (!input_file)
		return;

	fseek(input_file, SAFELOADER_PREAMBLE_SIZE, SEEK_SET);

	if (fread(buf, sizeof(buf), 1, input_file) != 1)
		error(1, errno, "Can not read image header");

	if (memcmp(HEADER_ID_QNEW, &buf[0], strlen(HEADER_ID_QNEW)) == 0)
		image->type = SAFELOADER_TYPE_QNEW;
	else if (memcmp(HEADER_ID_CLOUD, &buf[0], strlen(HEADER_ID_CLOUD)) == 0)
		image->type = SAFELOADER_TYPE_CLOUD;
	else if (ntohl(*((uint32_t *) &buf[0])) <= SAFELOADER_HEADER_SIZE)
		image->type = SAFELOADER_TYPE_VENDOR;
	else
		image->type = SAFELOADER_TYPE_DEFAULT;

	switch (image->type) {
	case SAFELOADER_TYPE_DEFAULT:
	case SAFELOADER_TYPE_VENDOR:
	case SAFELOADER_TYPE_CLOUD:
		image->payload_offset = SAFELOADER_PAYLOAD_OFFSET;
		break;
	case SAFELOADER_TYPE_QNEW:
		image->payload_offset = SAFELOADER_QNEW_PAYLOAD_OFFSET;
		break;
	}

	/* Parse image partition table */
	read_partition_table(input_file, image->payload_offset, &image->entries[0],
			     MAX_PARTITIONS, PARTITION_TABLE_FWUP);
}

static void write_partition(
		FILE *input_file,
		size_t firmware_offset,
		struct flash_partition_entry *entry,
		FILE *output_file)
{
	char buf[4096];
	size_t offset;

	fseek(input_file, entry->base + firmware_offset, SEEK_SET);

	for (offset = 0; sizeof(buf) + offset <= entry->size; offset += sizeof(buf)) {
		if (fread(buf, sizeof(buf), 1, input_file) != 1)
			error(1, errno, "Can not read partition from input_file");

		if (fwrite(buf, sizeof(buf), 1, output_file) != 1)
			error(1, errno, "Can not write partition to output_file");
	}
	/* write last chunk smaller than buffer */
	if (offset < entry->size) {
		offset = entry->size - offset;
		if (fread(buf, offset, 1, input_file) != 1)
			error(1, errno, "Can not read partition from input_file");
		if (fwrite(buf, offset, 1, output_file) != 1)
			error(1, errno, "Can not write partition to output_file");
	}
}

static int extract_firmware_partition(FILE *input_file, size_t firmware_offset, struct flash_partition_entry *entry, const char *output_directory)
{
	FILE *output_file;
	char output[PATH_MAX];

	snprintf(output, PATH_MAX, "%s/%s", output_directory, entry->name);
	output_file = fopen(output, "wb+");
	if (output_file == NULL) {
		error(1, errno, "Can not open output file %s", output);
	}

	write_partition(input_file, firmware_offset, entry, output_file);

	fclose(output_file);

	return 0;
}

/** extract all partitions from the firmware file */
static int extract_firmware(const char *input, const char *output_directory)
{
	struct safeloader_image_info info = {};
	struct stat statbuf;
	FILE *input_file;

	/* check input file */
	if (stat(input, &statbuf)) {
		error(1, errno, "Can not read input firmware %s", input);
	}

	/* check if output directory exists */
	if (stat(output_directory, &statbuf)) {
		error(1, errno, "Failed to stat output directory %s", output_directory);
	}

	if ((statbuf.st_mode & S_IFMT) != S_IFDIR) {
		error(1, errno, "Given output directory is not a directory %s", output_directory);
	}

	input_file = fopen(input, "rb");
	safeloader_parse_image(input_file, &info);

	for (size_t i = 0; i < MAX_PARTITIONS && info.entries[i].name; i++)
		extract_firmware_partition(input_file, info.payload_offset, &info.entries[i], output_directory);

	return 0;
}

static struct flash_partition_entry *find_partition(
		struct flash_partition_entry *entries, size_t max_entries,
		const char *name, const char *error_msg)
{
	for (size_t i = 0; i < max_entries; i++, entries++) {
		if (strcmp(entries->name, name) == 0)
			return entries;
	}

	if (error_msg) {
		error(1, 0, "%s", error_msg);
	}

	return NULL;
}

static int firmware_info(const char *input)
{
	struct safeloader_image_info info = {};
	struct image_partition_entry part = {};
	struct flash_partition_entry *e;
	FILE *input_file;

	input_file = fopen(input, "rb");

	safeloader_parse_image(input_file, &info);

	if (info.type == SAFELOADER_TYPE_VENDOR) {
		char buf[SAFELOADER_HEADER_SIZE] = {};
		uint32_t vendor_size;

		fseek(input_file, SAFELOADER_PREAMBLE_SIZE, SEEK_SET);
		fread(&vendor_size, sizeof(uint32_t), 1, input_file);

		vendor_size = ntohl(vendor_size);
		fread(buf, vendor_size, 1, input_file);

		printf("Firmware vendor string:\n");
		fwrite(buf, strnlen(buf, vendor_size), 1, stdout);
		printf("\n");
	}

	printf("Firmware image partitions:\n");
	printf("%-8s %-8s %s\n", "base", "size", "name");

	e = &info.entries[0];
	for (unsigned int i = 0; i < MAX_PARTITIONS && e->name; i++, e++)
		printf("%08x %08x %s\n", e->base, e->size, e->name);

	e = find_partition(&info.entries[0], MAX_PARTITIONS, "soft-version", NULL);
	if (e) {
		struct soft_version *s;
		unsigned int ascii_len;
		const uint8_t *buf;
		size_t data_len;
		bool isstr;

		safeloader_read_partition(input_file, info.payload_offset + e->base, e, &part);
		data_len = ntohl(((struct meta_header *) part.data)->length);
		buf = part.data + sizeof(struct meta_header);

		/* Check for (null-terminated) string */
		ascii_len = 0;
		while (ascii_len < data_len && isascii(buf[ascii_len]))
			ascii_len++;

		isstr = ascii_len == data_len;

		printf("\n[Software version]\n");
		if (isstr) {
			fwrite(buf, strnlen((const char *) buf, data_len), 1, stdout);
			putchar('\n');
		} else if (data_len >= offsetof(struct soft_version, rev)) {
			s = (struct soft_version *) buf;

			printf("Version: %d.%d.%d\n", s->version_major, s->version_minor, s->version_patch);
			printf("Date: %02x%02x-%02x-%02x\n", s->year_hi, s->year_lo, s->month, s->day);
			printf("Revision: %d\n", ntohl(s->rev));
		} else {
			printf("Failed to parse data\n");
		}

		free_image_partition(&part);
	}

	e = find_partition(&info.entries[0], MAX_PARTITIONS, "support-list", NULL);
	if (e) {
		size_t data_len;

		safeloader_read_partition(input_file, info.payload_offset + e->base, e, &part);
		data_len = ntohl(((struct meta_header *) part.data)->length);

		printf("\n[Support list]\n");
		fwrite(part.data + sizeof(struct meta_header), data_len, 1, stdout);
		printf("\n");

		free_image_partition(&part);
	}

	e = find_partition(&info.entries[0], MAX_PARTITIONS, "partition-table", NULL);
	if (e) {
		size_t flash_table_offset = info.payload_offset + e->base + 4;
		struct flash_partition_entry parts[MAX_PARTITIONS] = {};

		if (read_partition_table(input_file, flash_table_offset, parts, MAX_PARTITIONS, PARTITION_TABLE_FLASH))
			error(1, 0, "Error can not read the partition table (partition)");

		printf("\n[Partition table]\n");
		printf("%-8s %-8s %s\n", "base", "size", "name");

		e = &parts[0];
		for (unsigned int i = 0; i < MAX_PARTITIONS && e->name; i++, e++)
			printf("%08x %08x %s\n", e->base, e->size, e->name);
	}

	fclose(input_file);

	return 0;
}

static void write_ff(FILE *output_file, size_t size)
{
	char buf[4096];
	size_t offset;

	memset(buf, 0xff, sizeof(buf));

	for (offset = 0; offset + sizeof(buf) < size ; offset += sizeof(buf)) {
		if (fwrite(buf, sizeof(buf), 1, output_file) != 1)
			error(1, errno, "Can not write 0xff to output_file");
	}

	/* write last chunk smaller than buffer */
	if (offset < size) {
		offset = size - offset;
		if (fwrite(buf, offset, 1, output_file) != 1)
			error(1, errno, "Can not write partition to output_file");
	}
}

static void convert_firmware(const char *input, const char *output)
{
	struct flash_partition_entry flash[MAX_PARTITIONS] = {};
	struct flash_partition_entry *fwup_partition_table;
	struct flash_partition_entry *flash_file_system;
	struct flash_partition_entry *fwup_file_system;
	struct flash_partition_entry *flash_os_image;
	struct flash_partition_entry *fwup_os_image;
	struct safeloader_image_info info = {};
	size_t flash_table_offset;
	struct stat statbuf;
	FILE *output_file;
	FILE *input_file;

	/* check input file */
	if (stat(input, &statbuf)) {
		error(1, errno, "Can not read input firmware %s", input);
	}

	input_file = fopen(input, "rb");
	if (!input_file)
		error(1, 0, "Can not open input firmware %s", input);

	output_file = fopen(output, "wb");
	if (!output_file)
		error(1, 0, "Can not open output firmware %s", output);

	input_file = fopen(input, "rb");
	safeloader_parse_image(input_file, &info);

	fwup_os_image = find_partition(info.entries, MAX_PARTITIONS,
			"os-image", "Error can not find os-image partition (fwup)");
	fwup_file_system = find_partition(info.entries, MAX_PARTITIONS,
			"file-system", "Error can not find file-system partition (fwup)");
	fwup_partition_table = find_partition(info.entries, MAX_PARTITIONS,
			"partition-table", "Error can not find partition-table partition");

	/* the flash partition table has a 0x00000004 magic haeder */
	flash_table_offset = info.payload_offset + fwup_partition_table->base + 4;
	if (read_partition_table(input_file, flash_table_offset, flash, MAX_PARTITIONS, PARTITION_TABLE_FLASH) != 0)
		error(1, 0, "Error can not read the partition table (flash)");

	flash_os_image = find_partition(flash, MAX_PARTITIONS,
			"os-image", "Error can not find os-image partition (flash)");
	flash_file_system = find_partition(flash, MAX_PARTITIONS,
			"file-system", "Error can not find file-system partition (flash)");

	/* write os_image to 0x0 */
	write_partition(input_file, info.payload_offset, fwup_os_image, output_file);
	write_ff(output_file, flash_os_image->size - fwup_os_image->size);

	/* write file-system behind os_image */
	fseek(output_file, flash_file_system->base - flash_os_image->base, SEEK_SET);
	write_partition(input_file, info.payload_offset, fwup_file_system, output_file);

	fclose(output_file);
	fclose(input_file);
}

int main(int argc, char *argv[]) {
	const char *info_image = NULL, *board = NULL, *kernel_image = NULL, *rootfs_image = NULL, *output = NULL;
	const char *extract_image = NULL, *output_directory = NULL, *convert_image = NULL;
	bool add_jffs2_eof = false, sysupgrade = false;
	unsigned rev = 0;
	struct device_info *info;
	set_source_date_epoch();

	while (true) {
		int c;

		c = getopt(argc, argv, "i:B:k:r:o:V:jSh:x:d:z:");
		if (c == -1)
			break;

		switch (c) {
		case 'i':
			info_image = optarg;
			break;

		case 'B':
			board = optarg;
			break;

		case 'k':
			kernel_image = optarg;
			break;

		case 'r':
			rootfs_image = optarg;
			break;

		case 'o':
			output = optarg;
			break;

		case 'V':
			sscanf(optarg, "r%u", &rev);
			break;

		case 'j':
			add_jffs2_eof = true;
			break;

		case 'S':
			sysupgrade = true;
			break;

		case 'h':
			usage(argv[0]);
			return 0;

		case 'd':
			output_directory = optarg;
			break;

		case 'x':
			extract_image = optarg;
			break;

		case 'z':
			convert_image = optarg;
			break;

		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (info_image) {
		firmware_info(info_image);
	} else if (extract_image || output_directory) {
		if (!extract_image)
			error(1, 0, "No factory/oem image given via -x <file>. Output directory is only valid with -x");
		if (!output_directory)
			error(1, 0, "Can not extract an image without output directory. Use -d <dir>");
		extract_firmware(extract_image, output_directory);
	} else if (convert_image) {
		if (!output)
			error(1, 0, "Can not convert a factory/oem image into sysupgrade image without output file. Use -o <file>");
		convert_firmware(convert_image, output);
	} else {
		if (!board)
			error(1, 0, "no board has been specified");
		if (!kernel_image)
			error(1, 0, "no kernel image has been specified");
		if (!rootfs_image)
			error(1, 0, "no rootfs image has been specified");
		if (!output)
			error(1, 0, "no output filename has been specified");

		info = find_board(board);

		if (info == NULL)
			error(1, 0, "unsupported board %s", board);

		build_image(output, kernel_image, rootfs_image, rev, add_jffs2_eof, sysupgrade, info);
	}

	return 0;
}
