/*-------------------------------------------------------------------------
 *
 * etcd_fdw.h
 *	  Shared declarations for the etcd foreign-data wrapper.
 *
 *-------------------------------------------------------------------------
 */
#ifndef ETCD_FDW_H
#define ETCD_FDW_H

#include "postgres.h"

/* Logical role of a foreign-table column, resolved from its name. */
typedef enum EtcdColKind
{
	ETCD_COL_NONE = 0,			/* column not recognised; always NULL */
	ETCD_COL_KEY,
	ETCD_COL_VALUE,
	ETCD_COL_CREATE_REVISION,
	ETCD_COL_MOD_REVISION,
	ETCD_COL_VERSION,
	ETCD_COL_LEASE
} EtcdColKind;

#endif							/* ETCD_FDW_H */
