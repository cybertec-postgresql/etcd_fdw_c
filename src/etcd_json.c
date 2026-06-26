/*-------------------------------------------------------------------------
 *
 * etcd_json.c
 *	  base64 helpers and cJSON convenience wrappers for etcd_fdw.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/base64.h"
#include "utils/builtins.h"

#include "etcd_json.h"

/*
 * cJSON allocator hooks backed by palloc/pfree.  With custom hooks cJSON does
 * not use realloc (it falls back to allocate + copy + deallocate), so only
 * malloc/free wrappers are needed.  palloc never returns NULL (it errors on
 * OOM), and a partially-built tree is then freed by transaction abort.
 */
static void *
cjson_palloc(size_t sz)
{
	return palloc(sz);
}

static void
cjson_pfree(void *ptr)
{
	if (ptr != NULL)
		pfree(ptr);
}

void
etcd_json_init(void)
{
	cJSON_Hooks hooks;

	hooks.malloc_fn = cjson_palloc;
	hooks.free_fn = cjson_pfree;
	cJSON_InitHooks(&hooks);
}

char *
etcd_b64_encode(const char *data, int len)
{
	int			dstlen = pg_b64_enc_len(len);
	char	   *dst = palloc(dstlen + 1);
	int			n;

	/* src param is char* (<=PG17) or uint8* (PG18+); void* fits both */
	n = pg_b64_encode((const void *) data, len, dst, dstlen);
	if (n < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("etcd_fdw: base64 encoding failed")));
	dst[n] = '\0';
	return dst;
}

char *
etcd_b64_decode(const char *b64, int *out_len)
{
	int			srclen = (int) strlen(b64);
	int			dstlen = pg_b64_dec_len(srclen);
	char	   *dst = palloc(dstlen + 1);
	int			n;

	/* dst param is char* (<=PG17) or uint8* (PG18+); void* fits both */
	n = pg_b64_decode(b64, srclen, (void *) dst, dstlen);
	if (n < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("etcd_fdw: base64 decoding failed")));
	dst[n] = '\0';
	if (out_len)
		*out_len = n;
	return dst;
}

const char *
etcd_json_get_string(const cJSON *obj, const char *field)
{
	cJSON	   *item = cJSON_GetObjectItemCaseSensitive(obj, field);

	if (item && cJSON_IsString(item) && item->valuestring)
		return item->valuestring;
	return NULL;
}

int64
etcd_json_get_int64(const cJSON *obj, const char *field, int64 defval)
{
	cJSON	   *item = cJSON_GetObjectItemCaseSensitive(obj, field);

	if (item == NULL)
		return defval;

	/* etcd encodes 64-bit counters as JSON strings; tolerate numbers too. */
	if (cJSON_IsString(item) && item->valuestring)
		return (int64) strtoll(item->valuestring, NULL, 10);
	if (cJSON_IsNumber(item))
		return (int64) item->valuedouble;
	return defval;
}

char *
etcd_json_get_b64(const cJSON *obj, const char *field, int *out_len)
{
	const char *b64 = etcd_json_get_string(obj, field);

	if (b64 == NULL)
	{
		if (out_len)
			*out_len = 0;
		return NULL;
	}
	return etcd_b64_decode(b64, out_len);
}
