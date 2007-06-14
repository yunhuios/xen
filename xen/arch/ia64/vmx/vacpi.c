/*
 * vacpi.c: emulation of the ACPI
 * based on x86 hvm/pmtimer.c
 *
 * Copyright (c) 2007, FUJITSU LIMITED
 *      Kouya Shimura <kouya at jp fujitsu com>
 *
 * Copyright (c) 2007, XenSource inc.
 * Copyright (c) 2006, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307 USA.
 */

#include <asm/vmx_vcpu.h>
#include <asm/vmx.h>
#include <asm/hvm/vacpi.h>

/* The interesting bits of the PM1a_STS register */
#define TMR_STS    (1 << 0)
#define PWRBTN_STS (1 << 5)
#define GBL_STS    (1 << 8)

/* The same in PM1a_EN */
#define TMR_EN     (1 << 0)
#define PWRBTN_EN  (1 << 5)
#define GBL_EN     (1 << 8)

/* Mask of bits in PM1a_STS that can generate an SCI.  Although the ACPI
 * spec lists other bits, the PIIX4, which we are emulating, only
 * supports these three.  For now, we only use TMR_STS; in future we
 * will let qemu set the other bits */
#define SCI_MASK (TMR_STS|PWRBTN_STS|GBL_STS)

/* SCI IRQ number (must match SCI_INT number in ACPI FADT in hvmloader) */
#define SCI_IRQ 9

/* We provide a 32-bit counter (must match the TMR_VAL_EXT bit in the FADT) */
#define TMR_VAL_MASK  (0xffffffff)
#define TMR_VAL_MSB   (0x80000000)

/* Dispatch SCIs based on the PM1a_STS and PM1a_EN registers */
static void pmt_update_sci(struct domain *d, struct vacpi *s)
{
	if (s->regs.pm1a_en & s->regs.pm1a_sts & SCI_MASK)
		viosapic_set_irq(d, SCI_IRQ, 1);  /* Assert */
	else
		viosapic_set_irq(d, SCI_IRQ, 0);
}

/* Set the correct value in the timer, accounting for time elapsed
 * since the last time we did that. */
static void pmt_update_time(struct domain *d)
{
	struct vacpi *s = &d->arch.hvm_domain.vacpi;
	s_time_t curr_gtime;
	unsigned long delta;
	uint32_t msb = s->regs.tmr_val & TMR_VAL_MSB;

	/* Update the timer */
	curr_gtime = NOW();
	delta = curr_gtime - s->last_gtime;
	delta = ((delta >> 8) * ((FREQUENCE_PMTIMER << 32) / SECONDS(1))) >> 24;
	s->regs.tmr_val += delta;
	s->regs.tmr_val &= TMR_VAL_MASK;
	s->last_gtime = curr_gtime;

	/* If the counter's MSB has changed, set the status bit */
	if ((s->regs.tmr_val & TMR_VAL_MSB) != msb) {
		s->regs.pm1a_sts |= TMR_STS;
		pmt_update_sci(d, s);
	}
}

/* This function should be called soon after each time the MSB of the
 * pmtimer register rolls over, to make sure we update the status
 * registers and SCI at least once per rollover */
static void pmt_timer_callback(void *opaque)
{
	struct domain *d = opaque;
	struct vacpi *s = &d->arch.hvm_domain.vacpi;
	uint64_t cycles, time_flip;

	/* Recalculate the timer and make sure we get an SCI if we need one */
	pmt_update_time(d);

	/* How close are we to the next MSB flip? */
	cycles = TMR_VAL_MSB - (s->regs.tmr_val & (TMR_VAL_MSB - 1));

	/* Overall time between MSB flips */
	time_flip = (((SECONDS(1) << 23) / FREQUENCE_PMTIMER) * cycles) >> 23;

	/* Wake up again near the next bit-flip */
	set_timer(&s->timer, NOW() + time_flip + MILLISECS(1));
}

int vacpi_intercept(ioreq_t * iop, u64 * val)
{
	struct domain *d = current->domain;
	struct vacpi *s = &d->arch.hvm_domain.vacpi;
	uint64_t addr_off = iop->addr - ACPI_PM1A_EVT_BLK_ADDRESS;

	if (addr_off < 4) {	/* Access to PM1a_STS and PM1a_EN registers */
		void *p = (void *)&s->regs.evt_blk + addr_off;

		if (iop->dir == 1) {	/* Read */
			if (iop->size == 1)
				*val = *(uint8_t *) p;
			else if (iop->size == 2)
				*val = *(uint16_t *) p;
			else if (iop->size == 4)
				*val = *(uint32_t *) p;
			else
				panic_domain(NULL, "wrong ACPI "
					     "PM1A_EVT_BLK access\n");
		} else {	/* Write */
			uint8_t *sp = (uint8_t *) & iop->data;
			int i;

			for (i = 0; i < iop->size; i++, addr_off++, p++, sp++) {
				if (addr_off < 2) /* PM1a_STS */
					/* write-to-clear */
					*(uint8_t *) p &= ~*sp;
				else /* PM1a_EN */
					*(uint8_t *) p = *sp;
			}
			/* Fix the SCI state to match the new register state */
			pmt_update_sci(d, s);
		}

		iop->state = STATE_IORESP_READY;
		vmx_io_assist(current);
		return 1;
	}

	if (iop->addr == ACPI_PM_TMR_BLK_ADDRESS) {
		if (iop->size != 4)
			panic_domain(NULL, "wrong ACPI PM timer access\n");
		if (iop->dir == 1) {	/* Read */
			pmt_update_time(d);
			*val = s->regs.tmr_val;
		}
		/* PM_TMR_BLK is read-only */
		iop->state = STATE_IORESP_READY;
		vmx_io_assist(current);
		return 1;
	}

	return 0;
}

void vacpi_init(struct domain *d)
{
	struct vacpi *s = &d->arch.hvm_domain.vacpi;

	s->regs.tmr_val = 0;
	s->regs.evt_blk = 0;
	s->last_gtime = NOW();

	/* Set up callback to fire SCIs when the MSB of TMR_VAL changes */
	init_timer(&s->timer, pmt_timer_callback, d, first_cpu(cpu_online_map));
	pmt_timer_callback(d);
}

void vacpi_relinquish_resources(struct domain *d)
{
	struct vacpi *s = &d->arch.hvm_domain.vacpi;
	kill_timer(&s->timer);
}
