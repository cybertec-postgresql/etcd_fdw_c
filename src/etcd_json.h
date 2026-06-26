/*-------------------------------------------------------------------------
 *
 * etcd_json.h
 *	  base64 helpers and small cJSON convenience wrappers.
 *
 * etcd's JSON gateway encodes all keys/values as base64 and returns 64-bit
 * counters (revisions, versions, lease ids) as JSON *strings*.  These helpers
 * centralise that handling so callers deal in plain C buffers and int64.
 *
 *-------------------------------------------------------------------------
 */
#ifndef ETCD_FDW_JSON_H
#define ETCD_FDW_JSON_H

#include "postgres.h"
#include "cJSON.h"

/*
 * Route cJSON's allocations through palloc/pfree so every cJSON object is
 * owned by a PostgreSQL memory context and reclaimed on error/abort -- no
 * cJSON memory can leak.  Call once at module load (_PG_init).
 */
extern void etcd_json_init(void);

/*
 * base64.  Inputs/outputs are palloc'd in the current memory context.
 * etcd_b64_encode returns a NUL-terminated string.
 * etcd_b64_decode returns a buffer of *out_len bytes (also NUL-terminated for
 * convenience when the payload is textual); out_len may be NULL.
 */
extern char *etcd_b64_encode(const char *data, int len);
extern char *etcd_b64_decode(const char *b64, int *out_len);

/* cJSON field accessors that tolerate missing fields */
extern const char *etcd_json_get_string(const cJSON *obj, const char *field);
extern int64 etcd_json_get_int64(const cJSON *obj, const char *field,
								  int64 defval);

/*
 * Decode a base64 cJSON string field into a palloc'd byte buffer.  Returns
 * NULL if the field is absent.  *out_len receives the decoded length.
 */
extern char *etcd_json_get_b64(const cJSON *obj, const char *field,
							   int *out_len);

#endif							/* ETCD_FDW_JSON_H */
