// SPDX-License-Identifier: GPL-2.0
/*
 * Write, read back and delete one persistent object of the storage TA, so
 * that scripts can check secure storage across reboots, power loss and
 * rollback: each object holds a version number repeated over its whole
 * size, and a read reports the version only if every copy agrees.
 */

#include <err.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ta_storage.h>
#include <tee_api_defines.h>
#include <tee_api_defines_extensions.h>
#include <tee_client_api.h>
#include <util.h>

#include "storage_obj.h"

/* The storage TA copies the whole buffer into its 64 KiB heap */
#define STORAGE_OBJ_MAX_SIZE	(16 * 1024)

/* Exit codes, in addition to EXIT_SUCCESS and EXIT_FAILURE */
#define STORAGE_OBJ_NOT_FOUND	2
#define STORAGE_OBJ_INCONSISTENT 3

static void usage(void)
{
	fprintf(stderr,
		"usage: xtest --storage-obj <ree|rpmb> write <name> <version> [<size>]\n"
		"       xtest --storage-obj <ree|rpmb> read <name>\n"
		"       xtest --storage-obj <ree|rpmb> delete <name>\n"
		"read prints \"<name>: version <v>\" and exits 0, or exits %d if\n"
		"the object does not exist and %d if its copies of the version\n"
		"differ. <size> is a multiple of 4, at most %d bytes [4096].\n"
		"Objects belong to the storage TA, whose objects the regression\n"
		"tests enumerate: delete them after use.\n",
		STORAGE_OBJ_NOT_FOUND, STORAGE_OBJ_INCONSISTENT,
		STORAGE_OBJ_MAX_SIZE);
	exit(EXIT_FAILURE);
}

static TEEC_Result obj_open(TEEC_Session *sess, const char *name,
			    uint32_t flags, uint32_t storage_id, uint32_t *obj)
{
	TEEC_Operation op = { };
	TEEC_Result res = TEEC_ERROR_GENERIC;
	uint32_t org = 0;

	op.params[0].tmpref.buffer = (void *)name;
	op.params[0].tmpref.size = strlen(name);
	op.params[1].value.a = flags;
	op.params[2].value.a = storage_id;
	op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT,
					 TEEC_VALUE_INOUT, TEEC_VALUE_INPUT,
					 TEEC_NONE);
	res = TEEC_InvokeCommand(sess, TA_STORAGE_CMD_OPEN, &op, &org);
	if (!res)
		*obj = op.params[1].value.b;
	return res;
}

static TEEC_Result obj_cmd(TEEC_Session *sess, uint32_t cmd, uint32_t obj)
{
	TEEC_Operation op = { };
	uint32_t org = 0;

	op.params[0].value.a = obj;
	op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT, TEEC_NONE,
					 TEEC_NONE, TEEC_NONE);
	return TEEC_InvokeCommand(sess, cmd, &op, &org);
}

/* Create or atomically replace the object with its initial data */
static TEEC_Result obj_write(TEEC_Session *sess, const char *name,
			     uint32_t storage_id, void *data, size_t size)
{
	TEEC_Operation op = { };
	TEEC_Result res = TEEC_ERROR_GENERIC;
	uint32_t org = 0;

	op.params[0].tmpref.buffer = (void *)name;
	op.params[0].tmpref.size = strlen(name);
	op.params[1].value.a = TEE_DATA_FLAG_ACCESS_READ |
			       TEE_DATA_FLAG_ACCESS_WRITE |
			       TEE_DATA_FLAG_ACCESS_WRITE_META |
			       TEE_DATA_FLAG_OVERWRITE;
	op.params[2].value.b = storage_id;
	op.params[3].tmpref.buffer = data;
	op.params[3].tmpref.size = size;
	op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INPUT,
					 TEEC_VALUE_INOUT, TEEC_VALUE_INPUT,
					 TEEC_MEMREF_TEMP_INPUT);
	res = TEEC_InvokeCommand(sess, TA_STORAGE_CMD_CREATE, &op, &org);
	if (!res)
		res = obj_cmd(sess, TA_STORAGE_CMD_CLOSE, op.params[1].value.b);
	return res;
}

static TEEC_Result obj_read(TEEC_Session *sess, const char *name,
			    uint32_t storage_id, void *data, size_t *size)
{
	TEEC_Operation op = { };
	TEEC_Result res = TEEC_ERROR_GENERIC;
	uint32_t org = 0;
	uint32_t obj = 0;

	res = obj_open(sess, name, TEE_DATA_FLAG_ACCESS_READ, storage_id,
		       &obj);
	if (res)
		return res;

	op.params[0].tmpref.buffer = data;
	op.params[0].tmpref.size = *size;
	op.params[1].value.a = obj;
	op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_OUTPUT,
					 TEEC_VALUE_INOUT, TEEC_NONE,
					 TEEC_NONE);
	res = TEEC_InvokeCommand(sess, TA_STORAGE_CMD_READ, &op, &org);
	if (!res)
		*size = op.params[1].value.b;
	obj_cmd(sess, TA_STORAGE_CMD_CLOSE, obj);
	return res;
}

static TEEC_Result obj_delete(TEEC_Session *sess, const char *name,
			      uint32_t storage_id)
{
	TEEC_Result res = TEEC_ERROR_GENERIC;
	uint32_t obj = 0;

	res = obj_open(sess, name, TEE_DATA_FLAG_ACCESS_WRITE_META,
		       storage_id, &obj);
	if (res)
		return res;
	return obj_cmd(sess, TA_STORAGE_CMD_UNLINK, obj);
}

static unsigned long parse_ulong(const char *s, unsigned long max)
{
	unsigned long v = 0;
	char *end = NULL;

	v = strtoul(s, &end, 0);
	if (!*s || *end || v > max)
		usage();
	return v;
}

int storage_obj_runner_cmd_parser(int argc, char *argv[])
{
	TEEC_UUID uuid = TA_STORAGE_UUID;
	TEEC_Context ctx = { };
	TEEC_Session sess = { };
	TEEC_Result res = TEEC_ERROR_GENERIC;
	uint32_t storage_id = 0;
	uint32_t *data = NULL;
	const char *cmd = NULL;
	const char *name = NULL;
	size_t size = 4096;
	uint32_t version = 0;
	uint32_t eo = 0;
	size_t i = 0;
	int ret = EXIT_SUCCESS;

	/* argv[0] is "--storage-obj" */
	if (argc < 4)
		usage();
	if (!strcmp(argv[1], "ree"))
		storage_id = TEE_STORAGE_PRIVATE_REE;
	else if (!strcmp(argv[1], "rpmb"))
		storage_id = TEE_STORAGE_PRIVATE_RPMB;
	else
		usage();
	cmd = argv[2];
	name = argv[3];
	if (!strcmp(cmd, "write")) {
		if (argc != 5 && argc != 6)
			usage();
		version = parse_ulong(argv[4], UINT32_MAX);
		if (argc == 6)
			size = parse_ulong(argv[5], STORAGE_OBJ_MAX_SIZE);
		if (!size || size % sizeof(*data))
			usage();
	} else if (!strcmp(cmd, "read")) {
		if (argc != 4)
			usage();
		size = STORAGE_OBJ_MAX_SIZE;
	} else if (!strcmp(cmd, "delete")) {
		if (argc != 4)
			usage();
	} else {
		usage();
	}

	data = calloc(1, STORAGE_OBJ_MAX_SIZE);
	if (!data)
		err(EXIT_FAILURE, "calloc");

	res = TEEC_InitializeContext(NULL, &ctx);
	if (res)
		errx(EXIT_FAILURE, "TEEC_InitializeContext: %#"PRIx32, res);
	res = TEEC_OpenSession(&ctx, &sess, &uuid, TEEC_LOGIN_PUBLIC, NULL,
			       NULL, &eo);
	if (res)
		errx(EXIT_FAILURE,
		     "TEEC_OpenSession: res %#"PRIx32" err_orig %#"PRIx32,
		     res, eo);

	if (!strcmp(cmd, "write")) {
		for (i = 0; i < size / sizeof(*data); i++)
			data[i] = version;
		res = obj_write(&sess, name, storage_id, data, size);
		if (!res)
			printf("%s: version %"PRIu32", %zu bytes\n", name,
			       version, size);
	} else if (!strcmp(cmd, "read")) {
		res = obj_read(&sess, name, storage_id, data, &size);
		if (!res) {
			for (i = 1; i < size / sizeof(*data); i++)
				if (data[i] != data[0])
					break;
			if (!size || size % sizeof(*data) ||
			    i < size / sizeof(*data)) {
				printf("%s: inconsistent, %zu bytes\n", name,
				       size);
				ret = STORAGE_OBJ_INCONSISTENT;
			} else {
				printf("%s: version %"PRIu32", %zu bytes\n",
				       name, data[0], size);
			}
		}
	} else {
		res = obj_delete(&sess, name, storage_id);
		if (!res)
			printf("%s: deleted\n", name);
	}

	if (res == TEEC_ERROR_ITEM_NOT_FOUND) {
		printf("%s: not found\n", name);
		ret = STORAGE_OBJ_NOT_FOUND;
	} else if (res) {
		printf("%s: %s failed: %#"PRIx32"\n", name, cmd, res);
		ret = EXIT_FAILURE;
	}

	TEEC_CloseSession(&sess);
	TEEC_FinalizeContext(&ctx);
	free(data);
	return ret;
}
