/*
 * MongoDB BIND SDB Driver
 *
 * Copyright (C) 2011-2012 Tugdual de Kerviler <dekervit@gmail.com>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 *
 * $Id: mongodb.c,v 0.1 2011/06/12 23:33:00 tugdual Exp $
 */

#include <config.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "mongo.h"

#include <isc/mem.h>
#include <isc/print.h>
#include <isc/result.h>
#include <isc/util.h>

#include <dns/sdb.h>
#include <dns/result.h>

#include "named/globals.h"

#include "named/mongodb.h"

/*
 * collection mydomain.com :
 * [{ name:'mydomain.com', ttl: 259200, rdtype:'SOA', rdata:'mydomain.com. www.mydomain.com. 200309181 28800 7200 86400 28800' },
 *  { name:'mydomain.com', ttl: 259200, rdtype:'NS', rdata:'ns0.mydomain.com.' },
 *  { name:'mydomain.com', ttl: 259200, rdtype:'NS', rdata:'ns1.mydomain.com.' },
 *  { name:'mydomain.com', ttl: 259200, rdtype:'MX', rdata:'10 mail.mydomain.com.' },
 *  { name:'w0.mydomain.com', ttl: 259200, rdtype:'A', rdata:'192.168.1.1' },
 *  { name:'w1.mydomain.com', ttl: 259200, rdtype:'A', rdata:'192.168.1.2' },
 *  { name:'mydomain.com', ttl: 259200, rdtype:'CNAME', rdata:'w0.mydomain.com.' },
 *  { name:'mail.mydomain.com', ttl: 259200, rdtype:'CNAME', rdata:'w0.mydomain.com.' },
 *  { name:'ns0.mydomain.com', ttl: 259200, rdtype:'CNAME', rdata:'w0.mydomain.com.' },
 *  { name:'ns1.mydomain.com', ttl: 259200, rdtype:'CNAME', rdata:'w1.mydomain.com.' },
 *  { name:'www.mydomain.com', ttl: 259200, rdtype:'CNAME', rdata:'w0.mydomain.com.' },
 *  { name:'ftp.mydomain.com', ttl: 259200, rdtype:'CNAME', rdata:'w0.mydomain.com.' }
 * ]
 * Example entry in named.conf
 * ===========================
 *
 * zone "mydomain.com" {
 *	type master;
 *	notify no;
 *	database "mongodb databasename collectionname hostname port user password";
 * };
 *
 * Rebuilding the Server (modified from bind9/doc/misc/sdb)
 * =====================================================
 *
 * The driver module and header file (mongodb.c and mongodb.h)
 * must be copied to (or linked into)
 * the bind9/bin/named and bind9/bin/named/include directories
 * respectively, and must be added to the DBDRIVER_OBJS and DBDRIVER_SRCS
 * lines in bin/named/Makefile.in (e.g. add mongodb.c to DBDRIVER_SRCS and
 * mongodb.@O@ to DBDRIVER_OBJS).
 *
 * Add the results from the command `mysql_config --cflags` to DBDRIVER_INCLUDES.
 * (e.g. DBDRIVER_INCLUDES = -I'/root/mongo-c-driver/src')
 *
 * Add the results from the command `mysql_config --libs` to DBRIVER_LIBS.
 * (e.g. DBDRIVER_LIBS = -L'/root/mongo-c-driver' -lmongoc)
 *
 * In bind9/bin/named/main.c, add an include to mongodb.h
 * (e.g. #include "mongodb.h")  Then you must register the driver
 * in setup(), by adding mongodb_init(); before the call to ns_server_create().
 * Unregistration should be in cleanup(), by adding the call mongodb_clear();
 * after the call to ns_server_destroy().
 */

static dns_sdbimplementation_t *mongodb = NULL;

struct dbinfo
{
    mongo conn[1];
    char *database;
    char *collection;
    char *host;
    char *port;
    char *user;
    char *passwd;
};

static void mongodb_destroy(const char *zone, void *driverdata, void **dbdata);

/*
 * Connect to the database.
 */
static isc_result_t db_connect(struct dbinfo *dbi)
{
    if(mongo_connect(dbi->conn , dbi->host, atoi(dbi->port) ) != MONGO_OK)
        return (ISC_R_FAILURE);

    if(mongo_cmd_authenticate(dbi->conn, dbi->database, dbi->user, dbi->passwd) != MONGO_OK)
        return (ISC_R_FAILURE);

    return (ISC_R_SUCCESS);
}

/*
 * Check to see if the connection is still valid.  If not, attempt to reconnect.
 */
static isc_result_t maybe_reconnect(struct dbinfo *dbi)
{
    if(mongo_reconnect(dbi->conn) != MONGO_OK)
        return (ISC_R_FAILURE);

    if(mongo_cmd_authenticate(dbi->conn, dbi->database, dbi->user, dbi->passwd) != MONGO_OK)
        return (ISC_R_FAILURE);

    return (ISC_R_SUCCESS);
}


/*
 * This database operates on absolute names.
 */
static isc_result_t mongodb_lookup(const char *zone, const char *name, void *dbdata,
	                           dns_sdblookup_t *lookup)
{
    isc_result_t result;
    struct dbinfo *dbi = dbdata;
    bson_iterator it;
    mongo_cursor *cursor;
    bson query;
    const char * key, * rdata, *rdtype;
    int ttl;

    UNUSED(zone);

    char ns[1000];
    snprintf(ns, sizeof(ns), "%s.%s", dbi->database, dbi->collection);

    bson_init(&query);
    bson_append_string(&query, "name", name);
    bson_finish(&query);

    result = maybe_reconnect(dbi);
    if (result != ISC_R_SUCCESS)
        return (result);

    cursor = mongo_find(dbi->conn, ns, &query, NULL, 0, 0, 0);

    while( mongo_cursor_next(cursor) == MONGO_OK ) {
        bson_iterator_init(&it, cursor->current.data);
        while( bson_iterator_next(&it) ) {
            key = bson_iterator_key(&it);
            if(strcmp(key, "ttl") == 0) {
                ttl = bson_iterator_int(&it);
            } else if(strcmp(key, "rdtype") == 0) {
                rdtype = bson_iterator_string(&it);
            } else if(strcmp(key, "rdata") == 0) {
                rdata = bson_iterator_string(&it);
            }
        }
        result = dns_sdb_putrr(lookup, rdtype, ttl, rdata);
        if(result != ISC_R_SUCCESS) {
             return (ISC_R_FAILURE);
 		}
    }

    mongo_cursor_destroy(cursor);
    bson_destroy(&query);

	return (ISC_R_SUCCESS);
}


/*
 * Return all nodes in the database and fill the allnodes structure.
 */
static isc_result_t mongodb_allnodes(const char *zone, void *dbdata, dns_sdballnodes_t *allnodes)
{
    isc_result_t result;
    struct dbinfo *dbi = dbdata;
    bson_iterator it;
    mongo_cursor *cursor;
    const char * key, * name, * rdata, * rdtype;
    int ttl;

    UNUSED(zone);

    char ns[1000];
    snprintf(ns, sizeof(ns), "%s.%s", dbi->database, dbi->collection);

    result = maybe_reconnect(dbi);
    if (result != ISC_R_SUCCESS)
        return (result);

    cursor = mongo_find( dbi->conn, ns, bson_shared_empty(), bson_shared_empty(), 0, 0, 0);

    while( mongo_cursor_next(cursor) == MONGO_OK ) {
        bson_iterator_init(&it, cursor->current.data);
        while( bson_iterator_next(&it) ) {
            key = bson_iterator_key(&it);
            if(strcmp(key, "name") == 0) {
                name = bson_iterator_string(&it);
            } else if(strcmp(key, "ttl") == 0) {
                ttl = bson_iterator_int(&it);
            } else if(strcmp(key, "rdtype") == 0) {
                rdtype = bson_iterator_string(&it);
            } else if(strcmp(key, "rdata") == 0) {
                rdata = bson_iterator_string(&it);
            }
        }
        result = dns_sdb_putnamedrr(allnodes, name, rdtype, ttl, rdata);
    	if (result != ISC_R_SUCCESS) {
            return (ISC_R_FAILURE);
    	}
    }
    mongo_cursor_destroy(cursor);

    return (ISC_R_SUCCESS);
}

/*
 * Create a connection to the database and save any necessary information
 * in dbdata.
 *
 * argv[0] is the name of the database
 * argv[1] is the name of the collection
 * argv[2] (if present) is the name of the host to connect to
 * argv[3] (if present) is the name of the port to connect to
 * argv[4] (if present) is the name of the user to connect as
 * argv[5] (if present) is the name of the password to connect with
 */
static isc_result_t mongodb_create(const char *zone, int argc, char **argv,
	                           void *driverdata, void **dbdata)
{
    struct dbinfo *dbi;
    isc_result_t result;

    UNUSED(zone);
    UNUSED(driverdata);

    if (argc < 2)
        return (ISC_R_FAILURE);

    dbi = isc_mem_get(ns_g_mctx, sizeof(struct dbinfo));
    if (dbi == NULL)
        return (ISC_R_NOMEMORY);

    dbi->database   = NULL;
    dbi->collection = NULL;
    dbi->host       = NULL;
    dbi->port       = NULL;
    dbi->user       = NULL;
    dbi->passwd     = NULL;

#define STRDUP_OR_FAIL(target, source)			\
    do                                                  \
    {							\
	target = isc_mem_strdup(ns_g_mctx, source);	\
	if (target == NULL)                             \
        {				                \
            result = ISC_R_NOMEMORY;		        \
	    goto cleanup;				\
	}						\
    } while (0);

    STRDUP_OR_FAIL(dbi->database,   argv[0]);
    STRDUP_OR_FAIL(dbi->collection, argv[1]);
    STRDUP_OR_FAIL(dbi->host,       argv[2]);
    STRDUP_OR_FAIL(dbi->port,       argv[3]);
    STRDUP_OR_FAIL(dbi->user,       argv[4]);
    STRDUP_OR_FAIL(dbi->passwd,     argv[5]);

    result = db_connect(dbi);
    if (result != ISC_R_SUCCESS)
	    goto cleanup;

    *dbdata = dbi;
    return (ISC_R_SUCCESS);

cleanup:
    mongodb_destroy(zone, driverdata, (void **)&dbi);
    return (result);
}

/*
 * Close the connection to the database.
 */
static void mongodb_destroy(const char *zone, void *driverdata, void **dbdata)
{
    struct dbinfo *dbi = *dbdata;

    UNUSED(zone);
    UNUSED(driverdata);

    mongo_destroy(dbi->conn);

    if (dbi->database != NULL)
        isc_mem_free(ns_g_mctx, dbi->database);
    if (dbi->collection != NULL)
        isc_mem_free(ns_g_mctx, dbi->collection);
    if (dbi->host != NULL)
        isc_mem_free(ns_g_mctx, dbi->host);
    if (dbi->user != NULL)
        isc_mem_free(ns_g_mctx, dbi->user);
    if (dbi->passwd != NULL)
        isc_mem_free(ns_g_mctx, dbi->passwd);
    isc_mem_put(ns_g_mctx, dbi, sizeof(struct dbinfo));
}


static dns_sdbmethods_t mongodb_methods = {
    mongodb_lookup,
    NULL, /* authority */
    mongodb_allnodes,
    mongodb_create,
    mongodb_destroy
};

/*
 * Wrapper around dns_sdb_register().
 */
isc_result_t mongodb_init(void)
{
    unsigned int flags;
    flags = 0;
    return (dns_sdb_register("mongodb", &mongodb_methods, NULL, flags,
            ns_g_mctx, &mongodb));
}

/*
 * Wrapper around dns_sdb_unregister().
 */
void mongodb_clear(void)
{
    if (mongodb != NULL)
        dns_sdb_unregister(&mongodb);
}
