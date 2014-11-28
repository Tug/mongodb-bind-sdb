mongodb-bind-sdb
================

MongoDB sdb driver to use with bind9 DNS server. Modified from `bind9/doc/misc/sdb`.


## Example entry in named.conf

	zone "mydomain.com" {
		type master;
		notify no;
		database "mongodb databasename collectionname hostname port user password";
	};

## Example entries in mongodb

	[
		{ name:'mydomain.com', ttl: 259200, rdtype:'SOA', rdata:'mydomain.com. www.mydomain.com. 200309181 28800 7200 86400 28800' },
		{ name:'mydomain.com', ttl: 259200, rdtype:'NS', rdata:'ns0.mydomain.com.' },
		{ name:'mydomain.com', ttl: 259200, rdtype:'NS', rdata:'ns1.mydomain.com.' },
		{ name:'mydomain.com', ttl: 259200, rdtype:'MX', rdata:'10 mail.mydomain.com.' },
		{ name:'w0.mydomain.com', ttl: 259200, rdtype:'A', rdata:'192.168.1.1' },
		{ name:'w1.mydomain.com', ttl: 259200, rdtype:'A', rdata:'192.168.1.2' },
		{ name:'mydomain.com', ttl: 259200, rdtype:'CNAME', rdata:'w0.mydomain.com.' },
		{ name:'mail.mydomain.com', ttl: 259200, rdtype:'CNAME', rdata:'w0.mydomain.com.' },
		{ name:'ns0.mydomain.com', ttl: 259200, rdtype:'CNAME', rdata:'w0.mydomain.com.' },
		{ name:'ns1.mydomain.com', ttl: 259200, rdtype:'CNAME', rdata:'w1.mydomain.com.' },
		{ name:'www.mydomain.com', ttl: 259200, rdtype:'CNAME', rdata:'w0.mydomain.com.' },
		{ name:'ftp.mydomain.com', ttl: 259200, rdtype:'CNAME', rdata:'w0.mydomain.com.' }
	]


## Rebuilding the Server

The driver module and header file (`mongodb.c` and `mongodb.h`)
must be copied to (or linked into)
the `bind9/bin/named` and `bind9/bin/named/include` directories
respectively, and must be added to the `DBDRIVER_OBJS` and `DBDRIVER_SRCS`
lines in `bin/named/Makefile.in` (e.g. add `mongodb.c` to `DBDRIVER_SRCS` and
`mongodb.@O@` to `DBDRIVER_OBJS`).

Add the results from the command `mysql_config --cflags` to DBDRIVER_INCLUDES.
(e.g. `DBDRIVER_INCLUDES = -I'/root/mongo-c-driver/src'`)

Add the path of mongo-c-driver to DBDRIVER_LIBS.
(e.g. `DBDRIVER_LIBS = -L'/root/mongo-c-driver' -lmongoc`)

In `bind9/bin/named/main.c`, add an include to `mongodb.h`
(e.g. `#include "mongodb.h"`)  Then you must register the driver
in `setup()`, by adding `mongodb_init();` before the call to `ns_server_create()`.
Unregistration should be in `cleanup()`, by adding the call `mongodb_clear();`
after the call to `ns_server_destroy()`.
