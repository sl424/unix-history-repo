/*
 * Copyright (c) 1983 Regents of the University of California.
 * All rights reserved.  The Berkeley software License Agreement
 * specifies the terms and conditions for redistribution.
 */

#ifdef SYSLIBC_SCCS
_sccsid:.asciz	"@(#)geteuid.s	5.3 (Berkeley) %G%"
#endif SYSLIBC_SCCS

#include "SYS.h"

PSEUDO(geteuid,getuid)
	movl	r1,r0
	ret		# euid = geteuid();
