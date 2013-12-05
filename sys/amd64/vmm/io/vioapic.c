/*-
 * Copyright (c) 2013 Tycho Nightingale <tycho.nightingale@pluribusnetworks.com>
 * Copyright (c) 2013 Neel Natu <neel@freebsd.org>
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
 *
 * THIS SOFTWARE IS PROVIDED BY NETAPP, INC ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL NETAPP, INC OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/queue.h>
#include <sys/cpuset.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>

#include <x86/apicreg.h>
#include <machine/vmm.h>

#include "vmm_ktr.h"
#include "vmm_lapic.h"
#include "vioapic.h"

#define	IOREGSEL	0x00
#define	IOWIN		0x10

#define	REDIR_ENTRIES	24
#define	RTBL_RO_BITS	((uint64_t)(IOART_REM_IRR | IOART_DELIVS))

struct vioapic {
	struct vm	*vm;
	struct mtx	mtx;
	uint32_t	id;
	uint32_t	ioregsel;
	struct {
		uint64_t reg;
		int	 acnt;	/* sum of pin asserts (+1) and deasserts (-1) */
	} rtbl[REDIR_ENTRIES];
};

#define	VIOAPIC_LOCK(vioapic)		mtx_lock(&((vioapic)->mtx))
#define	VIOAPIC_UNLOCK(vioapic)		mtx_unlock(&((vioapic)->mtx))
#define	VIOAPIC_LOCKED(vioapic)		mtx_owned(&((vioapic)->mtx))

static MALLOC_DEFINE(M_VIOAPIC, "vioapic", "bhyve virtual ioapic");

#define	VIOAPIC_CTR1(vioapic, fmt, a1)					\
	VM_CTR1((vioapic)->vm, fmt, a1)

#define	VIOAPIC_CTR2(vioapic, fmt, a1, a2)				\
	VM_CTR2((vioapic)->vm, fmt, a1, a2)

#define	VIOAPIC_CTR3(vioapic, fmt, a1, a2, a3)				\
	VM_CTR3((vioapic)->vm, fmt, a1, a2, a3)

#define	VIOAPIC_CTR4(vioapic, fmt, a1, a2, a3, a4)			\
	VM_CTR4((vioapic)->vm, fmt, a1, a2, a3, a4)

#ifdef KTR
static const char *
pinstate_str(bool asserted)
{

	if (asserted)
		return ("asserted");
	else
		return ("deasserted");
}

static const char *
trigger_str(bool level)
{

	if (level)
		return ("level");
	else
		return ("edge");
}
#endif

static void
vioapic_send_intr(struct vioapic *vioapic, int pin)
{
	int vector, apicid, vcpuid;
	uint32_t low, high;
	cpuset_t dmask;
	bool level;

	KASSERT(pin >= 0 && pin < REDIR_ENTRIES,
	    ("vioapic_set_pinstate: invalid pin number %d", pin));

	KASSERT(VIOAPIC_LOCKED(vioapic),
	    ("vioapic_set_pinstate: vioapic is not locked"));

	low = vioapic->rtbl[pin].reg;
	high = vioapic->rtbl[pin].reg >> 32;

	/*
	 * XXX We only deal with:
	 * - physical destination
	 * - fixed delivery mode
	 */
	if ((low & IOART_DESTMOD) != IOART_DESTPHY) {
		VIOAPIC_CTR2(vioapic, "ioapic pin%d: unsupported dest mode "
		    "0x%08x", pin, low);
		return;
	}

	if ((low & IOART_DELMOD) != IOART_DELFIXED) {
		VIOAPIC_CTR2(vioapic, "ioapic pin%d: unsupported delivery mode "
		    "0x%08x", pin, low);
		return;
	}

	if ((low & IOART_INTMASK) == IOART_INTMSET) {
		VIOAPIC_CTR1(vioapic, "ioapic pin%d: masked", pin);
		return;
	}

	level = low & IOART_TRGRLVL ? true : false;
	if (level)
		vioapic->rtbl[pin].reg |= IOART_REM_IRR;

	vector = low & IOART_INTVEC;
	apicid = high >> APIC_ID_SHIFT;
	if (apicid != 0xff) {
		/* unicast */
		vcpuid = vm_apicid2vcpuid(vioapic->vm, apicid);
		VIOAPIC_CTR4(vioapic, "ioapic pin%d: %s triggered intr "
		    "vector %d on vcpuid %d", pin, trigger_str(level),
		    vector, vcpuid);
		lapic_set_intr(vioapic->vm, vcpuid, vector, level);
	} else {
		/* broadcast */
		VIOAPIC_CTR3(vioapic, "ioapic pin%d: %s triggered intr "
		    "vector %d on all vcpus", pin, trigger_str(level), vector);
		dmask = vm_active_cpus(vioapic->vm);
		while ((vcpuid = CPU_FFS(&dmask)) != 0) {
			vcpuid--;
			CPU_CLR(vcpuid, &dmask);
			lapic_set_intr(vioapic->vm, vcpuid, vector, level);
		}
	}
}

static void
vioapic_set_pinstate(struct vioapic *vioapic, int pin, bool newstate)
{
	int oldcnt, newcnt;
	bool needintr;

	KASSERT(pin >= 0 && pin < REDIR_ENTRIES,
	    ("vioapic_set_pinstate: invalid pin number %d", pin));

	KASSERT(VIOAPIC_LOCKED(vioapic),
	    ("vioapic_set_pinstate: vioapic is not locked"));

	oldcnt = vioapic->rtbl[pin].acnt;
	if (newstate)
		vioapic->rtbl[pin].acnt++;
	else
		vioapic->rtbl[pin].acnt--;
	newcnt = vioapic->rtbl[pin].acnt;

	if (newcnt < 0) {
		VIOAPIC_CTR2(vioapic, "ioapic pin%d: bad acnt %d",
		    pin, newcnt);
	}

	needintr = false;
	if (oldcnt == 0 && newcnt == 1) {
		needintr = true;
		VIOAPIC_CTR1(vioapic, "ioapic pin%d: asserted", pin);
	} else if (oldcnt == 1 && newcnt == 0) {
		VIOAPIC_CTR1(vioapic, "ioapic pin%d: deasserted", pin);
	} else {
		VIOAPIC_CTR3(vioapic, "ioapic pin%d: %s, ignored, acnt %d",
		    pin, pinstate_str(newstate), newcnt);
	}

	if (needintr)
		vioapic_send_intr(vioapic, pin);
}

enum irqstate {
	IRQSTATE_ASSERT,
	IRQSTATE_DEASSERT,
	IRQSTATE_PULSE
};

static int
vioapic_set_irqstate(struct vm *vm, int irq, enum irqstate irqstate)
{
	struct vioapic *vioapic;

	if (irq < 0 || irq >= REDIR_ENTRIES)
		return (EINVAL);

	vioapic = vm_ioapic(vm);

	VIOAPIC_LOCK(vioapic);
	switch (irqstate) {
	case IRQSTATE_ASSERT:
		vioapic_set_pinstate(vioapic, irq, true);
		break;
	case IRQSTATE_DEASSERT:
		vioapic_set_pinstate(vioapic, irq, false);
		break;
	case IRQSTATE_PULSE:
		vioapic_set_pinstate(vioapic, irq, true);
		vioapic_set_pinstate(vioapic, irq, false);
		break;
	default:
		panic("vioapic_set_irqstate: invalid irqstate %d", irqstate);
	}
	VIOAPIC_UNLOCK(vioapic);

	return (0);
}

int
vioapic_assert_irq(struct vm *vm, int irq)
{

	return (vioapic_set_irqstate(vm, irq, IRQSTATE_ASSERT));
}

int
vioapic_deassert_irq(struct vm *vm, int irq)
{

	return (vioapic_set_irqstate(vm, irq, IRQSTATE_DEASSERT));
}

int
vioapic_pulse_irq(struct vm *vm, int irq)
{

	return (vioapic_set_irqstate(vm, irq, IRQSTATE_PULSE));
}

static uint32_t
vioapic_read(struct vioapic *vioapic, uint32_t addr)
{
	int regnum, pin, rshift;

	regnum = addr & 0xff;
	switch (regnum) {
	case IOAPIC_ID:
		return (vioapic->id);
		break;
	case IOAPIC_VER:
		return (((REDIR_ENTRIES - 1) << MAXREDIRSHIFT) | 0x11);
		break;
	case IOAPIC_ARB:
		return (vioapic->id);
		break;
	default:
		break;
	}

	/* redirection table entries */
	if (regnum >= IOAPIC_REDTBL &&
	    regnum < IOAPIC_REDTBL + REDIR_ENTRIES * 2) {
		pin = (regnum - IOAPIC_REDTBL) / 2;
		if ((regnum - IOAPIC_REDTBL) % 2)
			rshift = 32;
		else
			rshift = 0;

		return (vioapic->rtbl[pin].reg >> rshift);
	}

	return (0);
}

static void
vioapic_write(struct vioapic *vioapic, uint32_t addr, uint32_t data)
{
	uint64_t data64, mask64;
	int regnum, pin, lshift;

	regnum = addr & 0xff;
	switch (regnum) {
	case IOAPIC_ID:
		vioapic->id = data & APIC_ID_MASK;
		break;
	case IOAPIC_VER:
	case IOAPIC_ARB:
		/* readonly */
		break;
	default:
		break;
	}

	/* redirection table entries */
	if (regnum >= IOAPIC_REDTBL &&
	    regnum < IOAPIC_REDTBL + REDIR_ENTRIES * 2) {
		pin = (regnum - IOAPIC_REDTBL) / 2;
		if ((regnum - IOAPIC_REDTBL) % 2)
			lshift = 32;
		else
			lshift = 0;

		data64 = (uint64_t)data << lshift;
		mask64 = (uint64_t)0xffffffff << lshift;
		vioapic->rtbl[pin].reg &= ~mask64 | RTBL_RO_BITS;
		vioapic->rtbl[pin].reg |= data64 & ~RTBL_RO_BITS;

		VIOAPIC_CTR2(vioapic, "ioapic pin%d: redir table entry %#lx",
		    pin, vioapic->rtbl[pin].reg);

		/*
		 * Generate an interrupt if the following conditions are met:
		 * - pin is not masked
		 * - previous interrupt has been EOIed
		 * - pin level is asserted
		 */
		if ((vioapic->rtbl[pin].reg & IOART_INTMASK) == IOART_INTMCLR &&
		    (vioapic->rtbl[pin].reg & IOART_REM_IRR) == 0 &&
		    (vioapic->rtbl[pin].acnt > 0)) {
			VIOAPIC_CTR2(vioapic, "ioapic pin%d: asserted at rtbl "
			    "write, acnt %d", pin, vioapic->rtbl[pin].acnt);
			vioapic_send_intr(vioapic, pin);
		}
	}
}

static int
vioapic_mmio_rw(struct vioapic *vioapic, uint64_t gpa, uint64_t *data,
    int size, bool doread)
{
	uint64_t offset;

	offset = gpa - VIOAPIC_BASE;

	/*
	 * The IOAPIC specification allows 32-bit wide accesses to the
	 * IOREGSEL (offset 0) and IOWIN (offset 16) registers.
	 */
	if (size != 4 || (offset != IOREGSEL && offset != IOWIN)) {
		if (doread)
			*data = 0;
		return (0);
	}

	VIOAPIC_LOCK(vioapic);
	if (offset == IOREGSEL) {
		if (doread)
			*data = vioapic->ioregsel;
		else
			vioapic->ioregsel = *data;
	} else {
		if (doread)
			*data = vioapic_read(vioapic, vioapic->ioregsel);
		else
			vioapic_write(vioapic, vioapic->ioregsel, *data);
	}
	VIOAPIC_UNLOCK(vioapic);

	return (0);
}

int
vioapic_mmio_read(void *vm, int vcpuid, uint64_t gpa, uint64_t *rval,
    int size, void *arg)
{
	int error;
	struct vioapic *vioapic;

	vioapic = vm_ioapic(vm);
	error = vioapic_mmio_rw(vioapic, gpa, rval, size, true);
	return (error);
}

int
vioapic_mmio_write(void *vm, int vcpuid, uint64_t gpa, uint64_t wval,
    int size, void *arg)
{
	int error;
	struct vioapic *vioapic;

	vioapic = vm_ioapic(vm);
	error = vioapic_mmio_rw(vioapic, gpa, &wval, size, false);
	return (error);
}

void
vioapic_process_eoi(struct vm *vm, int vcpuid, int vector)
{
	struct vioapic *vioapic;
	int pin;

	KASSERT(vector >= 0 && vector < 256,
	    ("vioapic_process_eoi: invalid vector %d", vector));

	vioapic = vm_ioapic(vm);
	VIOAPIC_CTR1(vioapic, "ioapic processing eoi for vector %d", vector);

	/*
	 * XXX keep track of the pins associated with this vector instead
	 * of iterating on every single pin each time.
	 */
	VIOAPIC_LOCK(vioapic);
	for (pin = 0; pin < REDIR_ENTRIES; pin++) {
		if ((vioapic->rtbl[pin].reg & IOART_REM_IRR) == 0)
			continue;
		if ((vioapic->rtbl[pin].reg & IOART_INTVEC) != vector)
			continue;
		vioapic->rtbl[pin].reg &= ~IOART_REM_IRR;
		if (vioapic->rtbl[pin].acnt > 0) {
			VIOAPIC_CTR2(vioapic, "ioapic pin%d: asserted at eoi, "
			    "acnt %d", pin, vioapic->rtbl[pin].acnt);
			vioapic_send_intr(vioapic, pin);
		}
	}
	VIOAPIC_UNLOCK(vioapic);
}

struct vioapic *
vioapic_init(struct vm *vm)
{
	int i;
	struct vioapic *vioapic;

	vioapic = malloc(sizeof(struct vioapic), M_VIOAPIC, M_WAITOK | M_ZERO);

	vioapic->vm = vm;
	mtx_init(&vioapic->mtx, "vioapic lock", NULL, MTX_DEF);

	/* Initialize all redirection entries to mask all interrupts */
	for (i = 0; i < REDIR_ENTRIES; i++)
		vioapic->rtbl[i].reg = 0x0001000000010000UL;

	return (vioapic);
}

void
vioapic_cleanup(struct vioapic *vioapic)
{

	free(vioapic, M_VIOAPIC);
}

int
vioapic_pincount(struct vm *vm)
{

	return (REDIR_ENTRIES);
}
