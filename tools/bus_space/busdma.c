/*-
 * Copyright (c) 2015 Marcel Moolenaar
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "busdma.h"

#include "../../sys/dev/proto/proto_dev.h"

struct obj {
	int	oid;
	u_int	type;
#define	OBJ_TYPE_NONE	0
#define	OBJ_TYPE_TAG	1
#define	OBJ_TYPE_MD	2
	u_int	refcnt;
	int	fd;
	struct obj *parent;
	u_long	key;
	union {
		struct {
			unsigned long	align;
			unsigned long	bndry;
			unsigned long	maxaddr;
			unsigned long	maxsz;
			unsigned long	maxsegsz;
			unsigned long	nsegs;
			unsigned long	datarate;
		} tag;
		struct {
		} md;
	} u;
};

static struct obj **oidtbl = NULL;
static int noids = 0;

static struct obj *
obj_alloc(u_int type)
{
	struct obj **newtbl, *obj;
	int oid;

	obj = malloc(sizeof(struct obj));
	obj->type = type;
	obj->refcnt = 0;

	for (oid = 0; oid < noids; oid++) {
		if (oidtbl[oid] == 0)
			break;
	}
	if (oid == noids) {
		newtbl = realloc(oidtbl, sizeof(struct obj *) * (noids + 1));
		if (newtbl == NULL) {
			free(obj);
			return (NULL);
		}
		oidtbl = newtbl;
		noids++;
	}
	oidtbl[oid] = obj;
	obj->oid = oid;
	return (obj);
}

static int
obj_free(struct obj *obj)
{

	oidtbl[obj->oid] = NULL;
	free(obj);
	return (0);
}

static struct obj *
obj_lookup(int oid, u_int type)
{
	struct obj *obj;

	if (oid < 0 || oid >= noids) {
		errno = EINVAL;
		return (NULL);
	}
	obj = oidtbl[oid];
	if (obj->refcnt == 0) {
		errno = ENXIO;
		return (NULL);
	}
	if (type != OBJ_TYPE_NONE && obj->type != type) {
		errno = ENODEV;
		return (NULL);
	}
	return (obj);
}

struct obj *
bd_tag_new(struct obj *ptag, int fd, u_long align, u_long bndry,
    u_long maxaddr, u_long maxsz, u_int nsegs, u_long maxsegsz,
    u_int datarate, u_int flags)
{
	struct proto_ioc_busdma ioc;
	struct obj *tag;

	tag = obj_alloc(OBJ_TYPE_TAG);
	if (tag == NULL)
		return (NULL);

	memset(&ioc, 0, sizeof(ioc));
	ioc.request = (ptag != NULL) ? PROTO_IOC_BUSDMA_TAG_DERIVE :
	    PROTO_IOC_BUSDMA_TAG_CREATE;
	ioc.key = (ptag != NULL) ? ptag->key : 0;
	ioc.u.tag.align = align;
	ioc.u.tag.bndry = bndry;
	ioc.u.tag.maxaddr = maxaddr;
	ioc.u.tag.maxsz = maxsz;
	ioc.u.tag.nsegs = nsegs;
	ioc.u.tag.maxsegsz = maxsegsz;
	ioc.u.tag.datarate = datarate;
	ioc.u.tag.flags = flags;
	if (ioctl(fd, PROTO_IOC_BUSDMA, &ioc) == -1) {
		obj_free(tag);
		return (NULL);
	}
	tag->refcnt = 1;
	tag->fd = fd;
	tag->parent = ptag;
	tag->key = ioc.result;
	tag->u.tag.align = ioc.u.tag.align;
	tag->u.tag.bndry = ioc.u.tag.bndry;
	tag->u.tag.maxaddr = ioc.u.tag.maxaddr;
	tag->u.tag.maxsz = ioc.u.tag.maxsz;
	tag->u.tag.maxsegsz = ioc.u.tag.maxsegsz;
	tag->u.tag.nsegs = ioc.u.tag.nsegs;
	tag->u.tag.datarate = ioc.u.tag.datarate;
	return (tag);
}

int
bd_tag_create(const char *dev, u_long align, u_long bndry, u_long maxaddr,
    u_long maxsz, u_int nsegs, u_long maxsegsz, u_int datarate, u_int flags)
{
	struct obj *tag;
	int fd;

	fd = open(dev, O_RDWR);
	if (fd == -1)
		return (-1);

	tag = bd_tag_new(NULL, fd, align, bndry, maxaddr, maxsz, nsegs,
	    maxsegsz, datarate, flags);
	if (tag == NULL) {
		close(fd);
		return (-1);
	}
	return (tag->oid);
}

int
bd_tag_derive(int ptid, u_long align, u_long bndry, u_long maxaddr,
    u_long maxsz, u_int nsegs, u_long maxsegsz, u_int datarate, u_int flags)
{
	struct obj *ptag, *tag;

	ptag = obj_lookup(ptid, OBJ_TYPE_TAG);
	if (ptag == NULL)
		return (-1);

	tag = bd_tag_new(ptag, ptag->fd, align, bndry, maxaddr, maxsz, nsegs,
	    maxsegsz, datarate, flags);
	if (tag == NULL)
		return (-1);
	ptag->refcnt++;
	return (tag->oid);
}

int
bd_tag_destroy(int tid)
{
	struct proto_ioc_busdma ioc;
	struct obj *ptag, *tag;

	tag = obj_lookup(tid, OBJ_TYPE_TAG);
	if (tag == NULL)
		return (errno);
	if (tag->refcnt > 1)
		return (EBUSY);

	memset(&ioc, 0, sizeof(ioc));
	ioc.request = PROTO_IOC_BUSDMA_TAG_DESTROY;
	ioc.key = tag->key;
	if (ioctl(tag->fd, PROTO_IOC_BUSDMA, &ioc) == -1)
		return (errno);

	if (tag->parent != NULL)
		tag->parent->refcnt--;
	else
		close(tag->fd);
	obj_free(tag);
	return (0);
}

int
bd_mem_alloc(int tid, u_int flags)
{
	struct proto_ioc_busdma ioc;
	struct obj *md, *tag;

	tag = obj_lookup(tid, OBJ_TYPE_TAG);
	if (tag == NULL)
		return (-1);

	md = obj_alloc(OBJ_TYPE_MD);
	if (md == NULL)
		return (-1);

	memset(&ioc, 0, sizeof(ioc));
	ioc.request = PROTO_IOC_BUSDMA_MEM_ALLOC;
	ioc.u.mem.tag = tag->key;
	ioc.u.mem.flags = flags;
	if (ioctl(tag->fd, PROTO_IOC_BUSDMA, &ioc) == -1) {
		obj_free(md);
		return (-1);
	}

	md->refcnt = 1;
	md->fd = tag->fd;
	md->parent = tag;
	tag->refcnt++;
	md->key = ioc.result;
	return (md->oid);
}

int
bd_mem_free(int mdid)
{
	struct proto_ioc_busdma ioc;
	struct obj *md;

	md = obj_lookup(mdid, OBJ_TYPE_MD);
	if (md == NULL)
		return (errno);

	memset(&ioc, 0, sizeof(ioc));
	ioc.request = PROTO_IOC_BUSDMA_MEM_FREE;
	ioc.key = md->key;
	if (ioctl(md->fd, PROTO_IOC_BUSDMA, &ioc) == -1)
		return (errno);

	md->parent->refcnt--;
	obj_free(md);
	return (0);
}
