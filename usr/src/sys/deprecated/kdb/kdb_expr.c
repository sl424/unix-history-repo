/*	kdb_expr.c	7.1	86/11/20	*/

#include "../kdb/defs.h"

char	*BADSYM;
char	*BADVAR;
char	*BADKET;
char	*BADSYN;
char	*NOCFN;
char	*NOADR;
char	*BADLOC;

ADDR	lastframe;
ADDR	savlastf;
ADDR	savframe;
ADDR	savpc;
ADDR	callpc;

char	*lp;
int	radix;
char	isymbol[1024];

char	lastc, peekc;

long	ditto;
long	expv;

/* term | term dyadic expr |  */
expr(a)
{
	register rc;
	register long lhs;

	rdc(); lp--; rc=term(a);

	while (rc) {
		lhs = expv;
		switch ((int)readchar()) {
		case '+':
			term(a|1); expv += lhs; break;
		case '-':
			term(a|1); expv = lhs - expv; break;
		case '#':
			term(a|1); expv = round(lhs,expv); break;
		case '*':
			term(a|1); expv *= lhs; break;
		case '%':
			term(a|1); expv = lhs/expv; break;
		case '&':
			term(a|1); expv &= lhs; break;
		case '|':
			term(a|1); expv |= lhs; break;
		case ')':
			if ((a&2)==0)
				error(BADKET);
		default:
			lp--;
			return (rc);
		}
	}
	return (rc);
}

/* item | monadic item | (expr) | */
static
term(a)
{

	switch ((int)readchar()) {
	case '*':
		term(a|1); expv=chkget(expv,DSP);
		return(1);
	case '@':
		term(a|1); expv=chkget(expv,ISP);
		return(1);
	case '-':
		term(a|1); expv = -expv;
		return(1);
	case '~':
		term(a|1); expv = ~expv;
		return(1);
	case '#':
		term(a|1); expv = !expv;
		return(1);
	case '(':
		expr(2);
		if (*lp!=')')
			error(BADSYN);
		lp++;
		return(1);
	}
	lp--;
	return (item(a));
}

/* name [ . local ] | number | . | ^ | <var | <register | 'x | | */
static
item(a)
{
	register base, d, regptr;
	char savc;
	register long frame;
	register struct nlist *symp;

	readchar();
	if (symchar(0)) {
		readsym();
		if (lastc=='.') {
			frame = pcb.pcb_fp; lastframe = 0;
			callpc= pcb.pcb_pc;
			while (errflg==0) {
			    savpc=callpc;
			    findsym(callpc,ISYM);
			    if (eqsym(cursym->n_un.n_name,isymbol,'~'))
				    break;
			    callpc=get(frame-8, DSP);
			    lastframe=frame;
			    frame=get(frame, DSP)&ALIGN;
			    if (frame==0)
				    error(NOCFN);
			}
			savlastf=lastframe; savframe=frame;
			readchar();
			if (symchar(0))
				chkloc(expv=frame);
		} else if ((symp=lookup(isymbol))==0)
			error(BADSYM);
		else
			expv = symp->n_value;
		lp--;
	} else if (getnum())
		;
	else if (lastc=='.') {
		readchar();
		if (symchar(0)) {
			lastframe=savlastf; callpc=savpc;
			chkloc(savframe);
		} else
			expv=dot;
		lp--;
	} else if (lastc=='"')
		expv=ditto;
	else if (lastc=='+')
		expv=inkdot(dotinc);
	else if (lastc=='^')
		expv=inkdot(-dotinc);
	else if (lastc=='<') {
		savc=rdc();
		if ((regptr=getreg(savc)) != -1)
			expv = *(int *)regptr;
		else if ((base=varchk(savc)) != -1)
			expv=var[base];
		else
			error(BADVAR);
	} else if (lastc=='\'') {
		d=4; expv=0;
		while (quotchar()) {
		    if (d--) {
		         expv <<= 8;
			 expv |= lastc;
		    } else
			error(BADSYN);
		}
	} else if (a)
		error(NOADR);
	else {
		lp--;
		return(0);
	}
	return (1);
}

/* service routines for expression reading */
static
getnum()
{
	register base,d,frpt;
	union { float r; long i;} real;

	if (!isdigit(lastc))
		return (0);
	if ((base = radix) < 0)
		base = -base;
	expv = 0;
	while (base>10 ? isxdigit(lastc) : isdigit(lastc)) {
		register m = MAXINT/base;

		if (expv>m)		/* avoid overflow */
			expv = (expv-m)*base+m*base;
		else
			expv *= base;
		if ((d=convdig(lastc))>=base || d<0)
			error(BADSYN);
		expv += d; readchar();
		if (expv==0) {
			if (lastc=='x' || lastc=='X') {
				 base=16; readchar();
			} else if (lastc=='t' || lastc=='T') {
				 base=10; readchar();
			} else if (lastc=='o' || lastc=='O') {
				 base=8; readchar();
			}
		}
	}
	if (lastc=='.' && (base==10 || expv==0)) {
		real.r=expv; frpt=0; base=10;
		while (isdigit(readchar())) {
			real.r *= base; frpt++;
			real.r += lastc-'0';
		}
		while (frpt--)
			real.r /= base;
		expv = real.i;
	}
	peekc=lastc;
	return (1);
}

static
readsym()
{
	register char *p;

	p = isymbol;
	do {
	    if (p < &isymbol[sizeof(isymbol)-1])
		    *p++ = lastc;
	    readchar();
	} while (symchar(1));
	*p++ = 0;
}

static
convdig(c)
	char c;
{
	if (isdigit(c))
		return (c-'0');
	if (isxdigit(c))
		return (c-'a'+10);
	return (-1);
}

static
symchar(dig)
{

	if (lastc=='\\') {
		readchar();
		return (TRUE);
	}
	return (isalpha(lastc) || lastc=='_' || dig && isdigit(lastc));
}

varchk(name)
	register name;
{
	if (isdigit(name))
		return (name-'0');
	if (isalpha(name))
		return ((name&037)-1+10);
	return (-1);
}

static
chkloc(frame)
	long frame;
{

	readsym();
	do {
		if (localsym(frame)==0)
			error(BADLOC);
		expv=localval;
	} while (!eqsym(cursym->n_un.n_name,isymbol,'~'));
}

eqsym(s1, s2, c)
	register char *s1, *s2;
{

	if (streq(s1,s2))
		return (1);
	if (*s1 == c && streq(s1+1, s2))
		return (1);
	return (0);
}

static
streq(s1, s2)
	char *s1, *s2;
{

	while (*s1 == *s2++)
		if (*s1++ == '\0')
			return (1);
	return (0);
}

static
round(a,b)
	register long a, b;
{
	register long w;

	w = (a/b)*b;
	if (a!=w)
		w += b;
	return (w);
}
