/*
 * Copyright (c) 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Tony Nardo of the Johns Hopkins University/Applied Physics Lab.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef lint
static char sccsid[] = "@(#)sprint.c	8.1 (Berkeley) 6/6/93";
#endif /* not lint */

#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <db.h>
#include <pwd.h>
#include <errno.h>
#include <utmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "finger.h"

static void	  stimeprint __P((WHERE *));

void
sflag_print()
{
	extern time_t now;
	extern int    oflag;
	register PERSON *pn;
	register WHERE *w;
	register int sflag, r;
	char p[80];
	DBT data, key;

	/*
	 * short format --
	 *	login name
	 *	real name
	 *	terminal name (the XX of ttyXX)
	 *	if terminal writeable (add an '*' to the terminal name
	 *		if not)
	 *	if logged in show idle time and day logged in, else
	 *		show last login date and time.
	 *		If > 6 months, show year instead of time.
	 *	if (-o)
	 *		office location
	 *		office phone
	 *	else
	 *		remote host
	 */
#define	MAXREALNAME	20
#define	MAXHOSTNAME	18	/* in reality, hosts are not longer than 16 */
	(void)printf("%-*s %-*s %s  %s\n", UT_NAMESIZE, "Login", MAXREALNAME,
	    "Name", "TTY   Idle  Login Time",
	    oflag ? " Office     Office Phone" : " Where");

	for (sflag = R_FIRST;; sflag = R_NEXT) {
		r = (*db->seq)(db, &key, &data, sflag);
		if (r == -1)
			err("db seq: %s", strerror(errno));
		if (r == 1)
			break;
		pn = *(PERSON **)data.data;

		for (w = pn->whead; w != NULL; w = w->next) {
			(void)printf("%-*.*s %-*.*s ", UT_NAMESIZE, UT_NAMESIZE,
			    pn->name, MAXREALNAME, MAXREALNAME,
			    pn->realname ? pn->realname : "");
			if (!w->loginat) {
				(void)printf("  *     *  No logins   ");
				goto office;
			}
			(void)putchar(w->info == LOGGEDIN && !w->writable ?
			    '*' : ' ');
			if (*w->tty)
				(void)printf("%-3.3s ",
					     (strncmp(w->tty, "tty", 3)
					      && strncmp(w->tty, "cua", 3))
					     ? w->tty : w->tty + 3);
			else
				(void)printf("    ");
			if (w->info == LOGGEDIN) {
				stimeprint(w);
				(void)printf("  ");
			} else
				(void)printf("    *  ");
			strftime(p, sizeof(p), "%c", localtime(&w->loginat));
#define SECSPERDAY 86400
#define DAYSPERWEEK 7
#define DAYSPERNYEAR 365
			if (now - w->loginat < SECSPERDAY * (DAYSPERWEEK - 1))
				(void)printf("%.3s   ", p);
			else
				(void)printf("%.6s", p + 4);
			if (now - w->loginat >= SECSPERDAY * DAYSPERNYEAR / 2)
				(void)printf("  %.4s", p + 20);
			else
				(void)printf(" %.5s", p + 11);
office:			if (oflag) {
				if (pn->office)
				(void)printf(" %-10.10s", pn->office);
			else if (pn->officephone)
				(void)printf(" %-10.10s", " ");
			if (pn->officephone)
				(void)printf(" %-.13s",
				    prphone(pn->officephone));
			} else
				(void)printf(" %.*s", MAXHOSTNAME, w->host);
			putchar('\n');
		}
	}
}

static void
stimeprint(w)
	WHERE *w;
{
	register struct tm *delta;

	delta = gmtime(&w->idletime);
	if (!delta->tm_yday)
		if (!delta->tm_hour)
			if (!delta->tm_min)
				(void)printf("     ");
			else
				(void)printf("%5d", delta->tm_min);
		else
			(void)printf("%2d:%02d",
			    delta->tm_hour, delta->tm_min);
	else
		(void)printf("%4dd", delta->tm_yday);
}
