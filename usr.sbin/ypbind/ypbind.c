/*
 * Copyright (c) 1992/3 Theo de Raadt <deraadt@fsa.ca>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef LINT
static char rcsid[] = "$Id: ypbind.c,v 1.8 1995/04/26 19:03:14 wpaul Exp $";
#endif

#include <sys/param.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <syslog.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <netdb.h>
#include <string.h>
#include <rpc/rpc.h>
#include <rpc/xdr.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <rpc/pmap_clnt.h>
#include <rpc/pmap_prot.h>
#include <rpc/pmap_rmt.h>
#include <unistd.h>
#include <stdlib.h>
#include <rpcsvc/yp_prot.h>
#include <rpcsvc/ypclnt.h>

#ifndef BINDINGDIR
#define BINDINGDIR "/var/yp/binding"
#endif

/*
 * Ping the server once every PING_INTERVAL seconds to make sure it's
 * still there.
 */
#ifndef PING_INTERVAL
#define PING_INTERVAL 60
#endif
#ifndef FAIL_THRESHOLD
#define FAIL_THRESHOLD 20
#endif

struct _dom_binding {
	struct _dom_binding *dom_pnext;
	char dom_domain[YPMAXDOMAIN + 1];
	struct sockaddr_in dom_server_addr;
	CLIENT *client_handle;
	long int dom_vers;
	int dom_lockfd;
	int dom_alive;
	int dom_broadcasting;
	int dom_default;
	time_t dom_check;
};

extern bool_t xdr_domainname(), xdr_ypbind_resp();
extern bool_t xdr_ypreq_key(), xdr_ypresp_val();
extern bool_t xdr_ypbind_setdom();

void	checkwork __P((void));
void	*ypbindproc_null_2 __P((SVCXPRT *, void *, CLIENT *));
bool_t	*ypbindproc_setdom_2 __P((SVCXPRT *, struct ypbind_setdom *, CLIENT *));
void	rpc_received __P((char *, struct sockaddr_in *, int ));
void	broadcast __P((struct _dom_binding *));
int	ping __P((struct _dom_binding *, int));
void	handle_children __P(( int ));

static char *broad_domain;
char *domainname;
struct _dom_binding *ypbindlist;

#define YPSET_NO	0
#define YPSET_LOCAL	1
#define YPSET_ALL	2
int ypsetmode = YPSET_NO;
int ypsecuremode = 0;

/* No more than MAX_CHILDREN child broadcasters at a time. */
#define MAX_CHILDREN 5
int child_fds[FD_SETSIZE];
static int fd[2];
int children = 0;

SVCXPRT *udptransp, *tcptransp;

void *
ypbindproc_null_2(transp, argp, clnt)
SVCXPRT *transp;
void *argp;
CLIENT *clnt;
{
	static char res;

	bzero((char *)&res, sizeof(res));
	return (void *)&res;
}

struct ypbind_resp *
ypbindproc_domain_2(transp, argp, clnt)
SVCXPRT *transp;
char *argp;
CLIENT *clnt;
{
	static struct ypbind_resp res;
	struct _dom_binding *ypdb;
	char path[MAXPATHLEN];

	bzero((char *)&res, sizeof res);
	res.ypbind_status = YPBIND_FAIL_VAL;
	res.ypbind_respbody.ypbind_error = YPBIND_ERR_NOSERV;

	for(ypdb=ypbindlist; ypdb; ypdb=ypdb->dom_pnext)
		if( strcmp(ypdb->dom_domain, argp) == 0)
			break;

	if(ypdb==NULL) {
		ypdb = (struct _dom_binding *)malloc(sizeof *ypdb);
		if (ypdb == NULL) {	
			syslog(LOG_WARNING, "malloc: %s", strerror(errno));
			res.ypbind_respbody.ypbind_error = YPBIND_ERR_RESC;
			return;
		}
		bzero((char *)ypdb, sizeof *ypdb);
		strncpy(ypdb->dom_domain, argp, sizeof ypdb->dom_domain);
		ypdb->dom_vers = YPVERS;
		ypdb->dom_alive = 0;
		ypdb->dom_default = 0;
		ypdb->dom_lockfd = -1;
		sprintf(path, "%s/%s.%ld", BINDINGDIR, ypdb->dom_domain, ypdb->dom_vers);
		unlink(path);
		ypdb->dom_pnext = ypbindlist;
		ypbindlist = ypdb;
		return &res;
	}

	if(ypdb->dom_alive==0)
		return &res;

	if (ping(ypdb, 1))
		return &res;

	res.ypbind_status = YPBIND_SUCC_VAL;
	res.ypbind_respbody.ypbind_error = 0; /* Success */
	res.ypbind_respbody.ypbind_bindinfo.ypbind_binding_addr.s_addr =
		ypdb->dom_server_addr.sin_addr.s_addr;
	res.ypbind_respbody.ypbind_bindinfo.ypbind_binding_port =
		ypdb->dom_server_addr.sin_port;
	/*printf("domain %s at %s/%d\n", ypdb->dom_domain,
		inet_ntoa(ypdb->dom_server_addr.sin_addr),
		ntohs(ypdb->dom_server_addr.sin_port));*/
	return &res;
}

bool_t *
ypbindproc_setdom_2(transp, argp, clnt)
SVCXPRT *transp;
struct ypbind_setdom *argp;
CLIENT *clnt;
{
	struct sockaddr_in *fromsin, bindsin;
	static char res;

	bzero((char *)&res, sizeof(res));
	fromsin = svc_getcaller(transp);

	switch(ypsetmode) {
	case YPSET_LOCAL:
		if( fromsin->sin_addr.s_addr != htonl(INADDR_LOOPBACK))
			return (void *)NULL;
		break;
	case YPSET_ALL:
		break;
	case YPSET_NO:
	default:
		return (void *)NULL;
	}

	if(ntohs(fromsin->sin_port) >= IPPORT_RESERVED)
		return (void *)&res;

	if(argp->ypsetdom_vers != YPVERS)
		return (void *)&res;

	bzero((char *)&bindsin, sizeof bindsin);
	bindsin.sin_family = AF_INET;
	bindsin.sin_addr.s_addr = argp->ypsetdom_addr.s_addr;
	bindsin.sin_port = argp->ypsetdom_port;
	rpc_received(argp->ypsetdom_domain, &bindsin, 1);

	res = 1;
	return (void *)&res;
}

static void
ypbindprog_2(rqstp, transp)
struct svc_req *rqstp;
register SVCXPRT *transp;
{
	union {
		char ypbindproc_domain_2_arg[MAXHOSTNAMELEN];
		struct ypbind_setdom ypbindproc_setdom_2_arg;
	} argument;
	struct authunix_parms *creds;
	char *result;
	bool_t (*xdr_argument)(), (*xdr_result)();
	char *(*local)();

	switch (rqstp->rq_proc) {
	case YPBINDPROC_NULL:
		xdr_argument = xdr_void;
		xdr_result = xdr_void;
		local = (char *(*)()) ypbindproc_null_2;
		break;

	case YPBINDPROC_DOMAIN:
		xdr_argument = xdr_domainname;
		xdr_result = xdr_ypbind_resp;
		local = (char *(*)()) ypbindproc_domain_2;
		break;

	case YPBINDPROC_SETDOM:
		switch(rqstp->rq_cred.oa_flavor) {
		case AUTH_UNIX:
			creds = (struct authunix_parms *)rqstp->rq_clntcred;
			if( creds->aup_uid != 0) {
				svcerr_auth(transp, AUTH_BADCRED);
				return;
			}
			break;
		default:
			svcerr_auth(transp, AUTH_TOOWEAK);
			return;
		}

		xdr_argument = xdr_ypbind_setdom;
		xdr_result = xdr_void;
		local = (char *(*)()) ypbindproc_setdom_2;
		break;

	default:
		svcerr_noproc(transp);
		return;
	}
	bzero((char *)&argument, sizeof(argument));
	if (!svc_getargs(transp, xdr_argument, &argument)) {
		svcerr_decode(transp);
		return;
	}
	result = (*local)(transp, &argument, rqstp);
	if (result != NULL && !svc_sendreply(transp, xdr_result, result)) {
		svcerr_systemerr(transp);
	}
	return;
}

/* Jack the reaper */
void reaper(sig)
int sig;
{
	int st;

	wait3(&st, WNOHANG, NULL);
}

void
main(argc, argv)
int argc;
char **argv;
{
	char path[MAXPATHLEN];
	struct timeval tv;
	fd_set fdsr;
	int i;

	yp_get_default_domain(&domainname);
	if( domainname[0] == '\0') {
		fprintf(stderr, "domainname not set. Aborting.\n");
		exit(1);
	}

	for(i=1; i<argc; i++) {
		if( strcmp("-ypset", argv[i]) == 0)
			ypsetmode = YPSET_ALL;
		else if (strcmp("-ypsetme", argv[i]) == 0)
		        ypsetmode = YPSET_LOCAL;
		else if (strcmp("-s", argv[i]) == 0)
		        ypsecuremode++;
	}

	/* blow away everything in BINDINGDIR */



#ifdef DAEMON
	switch(fork()) {
	case 0:
		break;
	case -1:
		perror("fork");
		exit(1);
	default:
		exit(0);
	}
	setsid();
#endif

	pmap_unset(YPBINDPROG, YPBINDVERS);

	udptransp = svcudp_create(RPC_ANYSOCK);
	if (udptransp == NULL) {
		fprintf(stderr, "cannot create udp service.");
		exit(1);
	}
	if (!svc_register(udptransp, YPBINDPROG, YPBINDVERS, ypbindprog_2,
	    IPPROTO_UDP)) {
		fprintf(stderr, "unable to register (YPBINDPROG, YPBINDVERS, udp).");
		exit(1);
	}

	tcptransp = svctcp_create(RPC_ANYSOCK, 0, 0);
	if (tcptransp == NULL) {
		fprintf(stderr, "cannot create tcp service.");
		exit(1);
	}

	if (!svc_register(tcptransp, YPBINDPROG, YPBINDVERS, ypbindprog_2,
	    IPPROTO_TCP)) {
		fprintf(stderr, "unable to register (YPBINDPROG, YPBINDVERS, tcp).");
		exit(1);
	}

	/* build initial domain binding, make it "unsuccessful" */
	ypbindlist = (struct _dom_binding *)malloc(sizeof *ypbindlist);
	if (ypbindlist == NULL) {
		perror("malloc");
		exit(1);
	}
	bzero((char *)ypbindlist, sizeof *ypbindlist);
	strncpy(ypbindlist->dom_domain, domainname, sizeof ypbindlist->dom_domain);
	ypbindlist->dom_vers = YPVERS;
	ypbindlist->dom_alive = 0;
	ypbindlist->dom_lockfd = -1;
	ypbindlist->client_handle = NULL;
	ypbindlist->dom_default = 1;
	sprintf(path, "%s/%s.%ld", BINDINGDIR, ypbindlist->dom_domain,
		ypbindlist->dom_vers);
	(void)unlink(path);

	/* Initialize children fds. */
	for (i = 0; i < FD_SETSIZE; i++)
		child_fds[i] = -1;

	openlog(argv[0], LOG_PID, LOG_DAEMON);

	while(1) {
		fdsr = svc_fdset;

		for (i = 0; i < FD_SETSIZE; i++)
			if (child_fds[i] > 0 )
				FD_SET(child_fds[i], &fdsr);

		tv.tv_sec = 1;
		tv.tv_usec = 0;

		switch(select(_rpc_dtablesize(), &fdsr, NULL, NULL, &tv)) {
		case 0:
			checkwork();
			reaper();
			break;
		case -1:
			syslog(LOG_WARNING, "select: %s", strerror(errno));
			break;
		default:
			for(i = 0; i < FD_SETSIZE; i++) {
				if (child_fds[i] > 0 && FD_ISSET(child_fds[i],&fdsr)) {
					handle_children(child_fds[i]);
					close(child_fds[i]);
					FD_CLR(child_fds[i], &fdsr);
					child_fds[i] = -1;
					children--;
					
				}
			}
			svc_getreqset(&fdsr);
			break;
		}
	}
}

void
checkwork()
{
	struct _dom_binding *ypdb;
	time_t t;

	time(&t);
	for(ypdb=ypbindlist; ypdb; ypdb=ypdb->dom_pnext) {
		if (!ypdb->dom_alive && !ypdb->dom_broadcasting) {
			if (!ypdb->dom_default)
				ypdb->dom_alive = 1;
			ypdb->dom_broadcasting = 1;
			broadcast(ypdb);
		}
		if (ypdb->dom_alive && ypdb->dom_check < t)
			ping(ypdb, 0);
	}
}

/* The clnt_broadcast() callback mechanism sucks. */

/*
 * Receive results from broadcaster. Don't worry about passing
 * bogus info to rpc_received() -- it can handle it.
 */
void handle_children(i)
int i;
{
	char buf[YPMAXDOMAIN + 1];
	struct sockaddr_in addr;

	if (read(i, &buf, sizeof(buf)) < 0)
		syslog(LOG_WARNING, "could not read from child: %s", strerror(errno));
	if (read(i, &addr, sizeof(struct sockaddr_in)) < 0)
		syslog(LOG_WARNING, "could not read from child: %s", strerror(errno));
	rpc_received((char *)&buf, &addr, 0);
}

/*
 * Send our dying words back to our parent before we perish.
 */
int
tell_parent(dom, addr)
char *dom;
struct sockaddr_in *addr;
{
	char buf[YPMAXDOMAIN + 1];
	struct timeval timeout;
	fd_set fds;

	timeout.tv_sec = 5;
	timeout.tv_usec = 0;

	sprintf (buf, "%s", broad_domain);
	if (write(fd[1], &buf, sizeof(buf)) < 0)
		return(1);

	/*
	 * Stay in sync with parent: wait for it to read our first
	 * message before sending the second.
	 */

	FD_ZERO(&fds);
	FD_SET(fd[1], &fds);
	if (select(FD_SETSIZE, NULL, &fds, NULL, &timeout) == -1)
		return(1);
	if (FD_ISSET(fd[1], &fds)) {
		if (write(fd[1], addr, sizeof(struct sockaddr_in)) < 0)
			return(1);
	} else {
		return(1);
	}

	close(fd[1]);
	return (0);
}

bool_t broadcast_result(out, addr)
bool_t *out;
struct sockaddr_in *addr;
{
	if (tell_parent(&broad_domain, addr))
		syslog(LOG_WARNING, "lost connection to parent");
	return TRUE;
}

/*
 * The right way to send RPC broadcasts.
 * Use the clnt_broadcast() RPC service. Unfortunately, clnt_broadcast()
 * blocks while waiting for replies, so we have to fork off seperate
 * broadcaster processes that do the waiting and then transmit their
 * results back to the parent for processing. We also have to remember
 * to save the name of the domain we're trying to bind in a global
 * variable since clnt_broadcast() provides no way to pass things to
 * the 'eachresult' callback function.
 */
void
broadcast(ypdb)
struct _dom_binding *ypdb;
{
	bool_t out = FALSE;
	enum clnt_stat stat;
	int i;

	if (children > MAX_CHILDREN)
		return;

	broad_domain = ypdb->dom_domain;

	if (pipe(fd) < 0) {
		syslog(LOG_WARNING, "pipe: %s",strerror(errno));
		return;
	}

	switch(fork()) {
	case 0:
		close(fd[0]);
		break;
	case -1:
		syslog(LOG_WARNING, "fork: %s", strerror(errno));
		close(fd[1]);
		close(fd[0]);
		return;
	default:
		for (i = 0; i < FD_SETSIZE; i++) {
			if (child_fds[i] < 0) {
				child_fds[i] = fd[0];
				break;
			}
		}
		close(fd[1]);
		children++;
		return;
	}

	close(ypdb->dom_lockfd);
	stat = clnt_broadcast(YPPROG, YPVERS, YPPROC_DOMAIN_NONACK,
	    xdr_domainname, (char *)ypdb->dom_domain, xdr_bool, (char *)&out,
	    broadcast_result);

	if (stat != RPC_SUCCESS) {
		syslog(LOG_WARNING, "NIS server for domain %s not responding",
			ypdb->dom_domain);
		bzero((char *)&ypdb->dom_server_addr,
						sizeof(struct sockaddr_in));
		if (tell_parent(&ypdb->dom_domain, &ypdb->dom_server_addr))
			syslog(LOG_WARNING, "lost connection to parent");
	}
	exit(0);
}

/*
 * The right way to check if a server is alive.
 * Attempt to get a client handle pointing to the server and send a
 * YPPROC_DOMAIN_NONACK. If we don't get a response, we invalidate
 * this binding entry, which will cause checkwork() to dispatch a
 * broadcaster process. Note that we treat non-default domains
 * specially: once bound, we keep tabs on our server, but if it
 * goes away and fails to respond after one round of broadcasting, we
 * abandon it until a client specifically references it again. We make
 * every effort to keep our default domain bound, however, since we
 * need it to keep the system on its feet.
 */
int
ping(ypdb, force)
struct _dom_binding *ypdb;
int force;
{
	bool_t out;
	struct timeval interval, timeout;
	enum clnt_stat stat;
	int rpcsock = RPC_ANYSOCK;
	time_t t;

	interval.tv_sec = 5;
	interval.tv_usec = 0;
	timeout.tv_sec = FAIL_THRESHOLD;
	timeout.tv_usec = 0;

	if (ypdb->dom_broadcasting)
		return(1);

	if (!ypdb->dom_default && ypdb->dom_vers == -1 && !force)
		return(1);

	if (ypdb->client_handle == NULL) {
		if ((ypdb->client_handle = clntudp_bufcreate(
			&ypdb->dom_server_addr, YPPROG, YPVERS,
			interval, &rpcsock, RPCSMALLMSGSIZE,
			RPCSMALLMSGSIZE)) == (CLIENT *)NULL) {
			/* Can't get a handle: we're dead. */
			ypdb->client_handle = NULL;
			ypdb->dom_alive = 0;
			ypdb->dom_vers = -1;
			flock(ypdb->dom_lockfd, LOCK_UN);
			return(1);
		}
	}

	if ((stat = clnt_call(ypdb->client_handle, YPPROC_DOMAIN_NONACK,
		xdr_domainname, (char *)ypdb->dom_domain,
		xdr_bool, (char *)&out, timeout)) != RPC_SUCCESS) {
		ypdb->client_handle = NULL;
		ypdb->dom_alive = 0;
		ypdb->dom_vers = -1;
		flock(ypdb->dom_lockfd, LOCK_UN);
		return(1);
	}
	/*
	 * We pinged successfully. Reset the timer.
	 */
	time(&t);
	ypdb->dom_check = t + PING_INTERVAL;

	return(0);
}

void rpc_received(dom, raddrp, force)
char *dom;
struct sockaddr_in *raddrp;
int force;
{
	struct _dom_binding *ypdb;
	struct iovec iov[2];
	struct ypbind_resp ybr;
	char path[MAXPATHLEN];
	int fd;

	/*printf("returned from %s/%d about %s\n", inet_ntoa(raddrp->sin_addr),
	       ntohs(raddrp->sin_port), dom);*/

	if(dom==NULL)
		return;

	for(ypdb=ypbindlist; ypdb; ypdb=ypdb->dom_pnext)
		if( strcmp(ypdb->dom_domain, dom) == 0)
			break;

	/* if in securemode, check originating port number */
	if (ypsecuremode && (ntohs(raddrp->sin_port) >= IPPORT_RESERVED)) {
	    syslog(LOG_WARNING, "Rejected NIS server on [%s/%d] for domain %s.",
		   inet_ntoa(raddrp->sin_addr), ntohs(raddrp->sin_port),
		   dom);
	    if (ypdb != NULL) {
		ypdb->dom_broadcasting = 0;
		ypdb->dom_alive = 0;
	    }
	    return;
	}

	if (raddrp->sin_addr.s_addr == (long)0) {
		ypdb->dom_broadcasting = 0;
		ypdb->dom_alive = 0;
		return;
	}

	if(ypdb==NULL) {
		if (force == 0)
			return;
		ypdb = (struct _dom_binding *)malloc(sizeof *ypdb);
		if (ypdb == NULL) {	
			syslog(LOG_WARNING, "malloc: %s", strerror(errno));
			return;
		}
		bzero((char *)ypdb, sizeof *ypdb);
		strncpy(ypdb->dom_domain, dom, sizeof ypdb->dom_domain);
		ypdb->dom_lockfd = -1;
		ypdb->dom_default = 0;
		ypdb->dom_alive = 0;
		ypdb->dom_broadcasting = 0;
		ypdb->dom_pnext = ypbindlist;
		ypbindlist = ypdb;
	}

	/* We've recovered from a crash: inform the world. */
	if (ypdb->dom_vers = -1 && ypdb->dom_server_addr.sin_addr.s_addr)
		syslog(LOG_WARNING, "NIS server for domain %s OK",
							ypdb->dom_domain);

	bcopy((char *)raddrp, (char *)&ypdb->dom_server_addr,
		sizeof ypdb->dom_server_addr);

	ypdb->dom_vers = YPVERS;
	ypdb->dom_alive = 1;
	ypdb->dom_broadcasting = 0;

	if(ypdb->dom_lockfd != -1)
		close(ypdb->dom_lockfd);

	sprintf(path, "%s/%s.%ld", BINDINGDIR,
		ypdb->dom_domain, ypdb->dom_vers);
#ifdef O_SHLOCK
	if( (fd=open(path, O_CREAT|O_SHLOCK|O_RDWR|O_TRUNC, 0644)) == -1) {
		(void)mkdir(BINDINGDIR, 0755);
		if( (fd=open(path, O_CREAT|O_SHLOCK|O_RDWR|O_TRUNC, 0644)) == -1)
			return;
	}
#else
	if( (fd=open(path, O_CREAT|O_RDWR|O_TRUNC, 0644)) == -1) {
		(void)mkdir(BINDINGDIR, 0755);
		if( (fd=open(path, O_CREAT|O_RDWR|O_TRUNC, 0644)) == -1)
			return;
	}
	flock(fd, LOCK_SH);
#endif

	/*
	 * ok, if BINDINGDIR exists, and we can create the binding file,
	 * then write to it..
	 */
	ypdb->dom_lockfd = fd;

	iov[0].iov_base = (caddr_t)&(udptransp->xp_port);
	iov[0].iov_len = sizeof udptransp->xp_port;
	iov[1].iov_base = (caddr_t)&ybr;
	iov[1].iov_len = sizeof ybr;

	bzero(&ybr, sizeof ybr);
	ybr.ypbind_status = YPBIND_SUCC_VAL;
	ybr.ypbind_respbody.ypbind_bindinfo.ypbind_binding_addr = raddrp->sin_addr;
	ybr.ypbind_respbody.ypbind_bindinfo.ypbind_binding_port = raddrp->sin_port;

	if( writev(ypdb->dom_lockfd, iov, 2) != iov[0].iov_len + iov[1].iov_len) {
		syslog(LOG_WARNING, "write: %s", strerror(errno));
		close(ypdb->dom_lockfd);
		ypdb->dom_lockfd = -1;
		return;
	}
}
