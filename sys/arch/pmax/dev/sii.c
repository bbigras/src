/*	$NetBSD: sii.c,v 1.12.4.2 1996/09/09 20:24:36 thorpej Exp $	*/

/*-
 * Copyright (c) 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Ralph Campbell and Rick Macklem.
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
 *
 *	@(#)sii.c	8.2 (Berkeley) 11/30/93
 *
 * from: Header: /sprite/src/kernel/dev/ds3100.md/RCS/devSII.c,
 *	v 9.2 89/09/14 13:37:41 jhh Exp $ SPRITE (DECWRL)";
 */

#include "sii.h"
#if NSII > 0
/*
 * SCSI interface driver
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/dkstat.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/device.h>

#ifdef notyet
#include <scsi/scsi_all.h>
#include <scsi/scsiconf.h>
#endif

#include <machine/autoconf.h>
#include <machine/machConst.h>
#include <pmax/dev/device.h>
#include <pmax/dev/scsi.h>
#include <pmax/dev/siireg.h>

#include <pmax/pmax/kn01.h>

typedef struct scsi_state {
	int	statusByte;	/* status byte returned during STATUS_PHASE */
	int	dmaDataPhase;	/* which data phase to expect */
	int	dmaCurPhase;	/* SCSI phase if DMA is in progress */
	int	dmaPrevPhase;	/* SCSI phase of DMA suspended by disconnect */
	u_short	*dmaAddr[2];	/* DMA buffer memory address */
	int	dmaBufIndex;	/* which of the above is currently in use */
	int	dmalen;		/* amount to transfer in this chunk */
	int	cmdlen;		/* total remaining amount of cmd to transfer */
	u_char	*cmd;		/* current pointer within scsicmd->cmd */
	int	buflen;		/* total remaining amount of data to transfer */
	char	*buf;		/* current pointer within scsicmd->buf */
	u_short	flags;		/* see below */
	u_short	prevComm;	/* command reg before disconnect */
	u_short	dmaCtrl;	/* DMA control register if disconnect */
	u_short	dmaAddrL;	/* DMA address register if disconnect */
	u_short	dmaAddrH;	/* DMA address register if disconnect */
	u_short	dmaCnt;		/* DMA count if disconnect */
	u_short	dmaByte;	/* DMA byte if disconnect on odd boundary */
	u_short	dmaReqAck;	/* DMA synchronous xfer offset or 0 if async */
} State;

/* state flags */
#define FIRST_DMA	0x01	/* true if no data DMA started yet */
#define PARITY_ERR	0x02	/* true if parity error seen */

#define SII_NCMD	7
struct siisoftc {
	struct device sc_dev;		/* us as a device */
	SIIRegs	*sc_regs;		/* HW address of SII controller chip */
	int	sc_flags;
	int	sc_target;		/* target SCSI ID if connected */
	ScsiCmd	*sc_cmd[SII_NCMD];	/* active command indexed by ID */
	State	sc_st[SII_NCMD];	/* state info for each active command */
#ifdef NEW_SCSI
	struct scsi_link sc_link;		/* scsi lint struct */
#endif
};

/*
 * Device definition for autoconfiguration.
 * 
 */
int	siimatch  __P((struct device * parent, void *cfdata, void *aux));
void	siiattach __P((struct device *parent, struct device *self, void *aux));
int	siiprint(void*, const char*);

int sii_doprobe __P((void *addr, int unit, int flags, int pri,
		     struct device *self));
int siiintr __P((void *sc));

extern struct cfdriver sii_cd;

struct cfattach sii_ca = {
	sizeof(struct siisoftc), siimatch, siiattach
};

struct  cfdriver sii_cd = {
	NULL, "sii", DV_DULL
};

#ifdef USE_NEW_SCSI
/* Glue to the machine-independent scsi */
struct scsi_adapter asc_switch = {
	NULL, /* XXX - asc_scsi_cmd */
#if 0
/*XXX*/	minphys,		/* no max transfer size; DMA engine deals */
#else
	SII_MAX_DMA_XFER_LENGTH,
#endif
	NULL,
	NULL,
};

struct scsi_device sii_dev = {
/*XXX*/	NULL,			/* Use default error handler */
/*XXX*/	NULL,			/* have a queue, served by this */
/*XXX*/	NULL,			/* have no async handler */
/*XXX*/	NULL,			/* Use default 'done' routine */
};
#endif

/*
 * Definition of the controller for the old auto-configuration program
 * and old-style pmax scsi drivers.
 */
void	siistart();
struct	pmax_driver siidriver = {
	"sii", NULL, siistart, 0,
};


/*
 * MACROS for timing out spin loops.
 *
 * Wait until expression is true.
 *
 * Control register bits can change at any time so when the CPU
 * reads a register, the bits might change and
 * invalidate the setup and hold times for the CPU.
 * This macro reads the register twice to be sure the value is stable.
 *
 *	args:	var 		- variable to save control register contents
 *		reg		- control register to read
 *		expr 		- expression to spin on
 *		spincount 	- maximum number of times through the loop
 *		cntr		- variable for number of tries
 */
#define	SII_WAIT_UNTIL(var, reg, expr, spincount, cntr) {	\
		register u_int tmp = reg;			\
		for (cntr = 0; cntr < spincount; cntr++) {	\
			while (tmp != (var = reg))		\
				tmp = var;			\
			if (expr)				\
				break;				\
			if (cntr >= 100)			\
				DELAY(100);			\
		}						\
	}

#ifdef DEBUG
int	sii_debug = 1;
int	sii_debug_cmd;
int	sii_debug_bn;
int	sii_debug_sz;
#define NLOG 16
struct sii_log {
	u_short	cstat;
	u_short	dstat;
	u_short	comm;
	u_short	msg;
	int	rlen;
	int	dlen;
	int	target;
} sii_log[NLOG], *sii_logp = sii_log;
#endif

u_char	sii_buf[256];	/* used for extended messages */

#define NORESET	0
#define RESET	1
#define NOWAIT	0
#define WAIT	1

/* define a safe address in the SCSI buffer for doing status & message DMA */
#define SII_BUF_ADDR	(MACH_PHYS_TO_UNCACHED(KN01_SYS_SII_B_START) \
		+ SII_MAX_DMA_XFER_LENGTH * 14)


/*
 * Other forward references
 */

static void sii_Reset __P((register struct siisoftc *sc, int resetbus));
static void sii_StartCmd __P((register struct siisoftc *sc, int target));
static void sii_CmdDone __P((register struct siisoftc *sc, int target,
			     int error));
static void sii_DoIntr __P((register struct siisoftc *sc, u_int dstat));
static void sii_StateChg __P((register struct siisoftc *sc, u_int cstat));
static int  sii_GetByte __P((register SIIRegs *regs, int phase, int ack));
static void sii_DoSync __P((register SIIRegs *regs, register State *state));
static void sii_StartDMA __P((register SIIRegs *regs, int phase,
			      u_short *dmaAddr, int size));

void siistart __P((register ScsiCmd *scsicmd));
void sii_DumpLog __P((void));

void  CopyToBuffer __P((u_short *src, 	/* NOTE: must be short aligned */
			volatile u_short *dst, int length));
void CopyFromBuffer __P((volatile u_short *src, char *dst, int length));



/*
 * Match driver based on name
 */
int
siimatch(parent, match, aux)
	struct device *parent;
	void *match;
	void *aux;
{
	struct confargs *ca = aux;

	if (strcmp(ca->ca_name, "sii") != 0 &&
	    strncmp(ca->ca_name, "PMAZ-AA ", 8) != 0) /*XXX*/
		return (0);

	/* XXX check for bad address */
	/*
	 * XXX KN01s (3100/2100) have exactly one SII.
	 * the Decsystem 5100 apparently uses them also, but as yet we
	 * don't know at what address.
	 *
	 * XXX  PVAXES apparently use the SII also.
	 */
	return (1);
}

void
siiattach(parent, self, aux)
	struct device *parent;
	struct device *self;
	void *aux;
{
	register struct confargs *ca = aux;
	register struct siisoftc *sc = (struct siisoftc *) self;
	register void *siiaddr;
	register int i;

	siiaddr = (void*)MACH_PHYS_TO_UNCACHED(ca->ca_addr);

	sc->sc_regs = (SIIRegs *)siiaddr;
	sc->sc_flags = sc->sc_dev.dv_cfdata->cf_flags;
	sc->sc_target = -1;	/* no command active */
	/*
	 * Give each target its own DMA buffer region.
	 * Make it big enough for 2 max transfers so we can ping pong buffers
	 * while we copy the data.
	 */
	for (i = 0; i < SII_NCMD; i++) {
		sc->sc_st[i].dmaAddr[0] = (u_short *)
			MACH_PHYS_TO_UNCACHED(KN01_SYS_SII_B_START) +
			2 * SII_MAX_DMA_XFER_LENGTH * i;
		sc->sc_st[i].dmaAddr[1] = sc->sc_st[i].dmaAddr[0] +
			SII_MAX_DMA_XFER_LENGTH;
	}

	/* Hack for old-sytle SCSI-device probe */
	(void) pmax_add_scsi(&siidriver, sc->sc_dev.dv_unit);

	sii_Reset(sc, RESET);

	/*priority = ca->ca_slot;*/
	/* tie pseudo-slot to device */
	BUS_INTR_ESTABLISH(ca, siiintr, sc);
	printf("\n");
}

/*
 * Start activity on a SCSI device.
 * We maintain information on each device separately since devices can
 * connect/disconnect during an operation.
 */
void
siistart(scsicmd)
	register ScsiCmd *scsicmd;	/* command to start */
{
	register struct pmax_scsi_device *sdp = scsicmd->sd;
	register struct siisoftc *sc = sii_cd.cd_devs[sdp->sd_ctlr];
	int s;

	s = splbio();
	/*
	 * Check if another command is already in progress.
	 * We may have to change this if we allow SCSI devices with
	 * separate LUNs.
	 */
	if (sc->sc_cmd[sdp->sd_drive]) {
		printf("%s: device %s busy at start\n", sc->sc_dev.dv_xname,
			sdp->sd_driver->d_name);
		(*sdp->sd_driver->d_done)(scsicmd->unit, EBUSY,
			scsicmd->buflen, 0);
		splx(s);
	}
	sc->sc_cmd[sdp->sd_drive] = scsicmd;
	sii_StartCmd(sc, sdp->sd_drive);
	splx(s);
}

/*
 * Check to see if any SII chips have pending interrupts
 * and process as appropriate.
 */
int
siiintr(xxxsc)
	void *xxxsc;
{
	register struct siisoftc *sc = xxxsc;
	u_int dstat;

	/*
	 * Find which controller caused the interrupt.
	 */
	dstat = sc->sc_regs->dstat;
	if (dstat & (SII_CI | SII_DI)) {
		sii_DoIntr(sc, dstat);
		return (0);	/* XXX */
	}

	return (1);		/* XXX spurious interrupt? */
}

/*
 * Reset the SII chip and do a SCSI reset if 'reset' is true.
 * NOTE: if !cold && reset, should probably probe for devices
 * since a SCSI bus reset will set UNIT_ATTENTION.
 */
static void
sii_Reset(sc, reset)
	register struct siisoftc* sc;
	int reset;				/* TRUE => reset SCSI bus */
{
	register SIIRegs *regs = sc->sc_regs;

#ifdef DEBUG
	if (sii_debug > 1)
		printf("sii: RESET\n");
#endif
	/*
	 * Reset the SII chip.
	 */
	regs->comm = SII_CHRESET;
	/*
	 * Set arbitrated bus mode.
	 */
	regs->csr = SII_HPM;
	/*
	 * SII is always ID 7.
	 */
	regs->id = SII_ID_IO | 7;
	/*
	 * Enable SII to drive the SCSI bus.
	 */
	regs->dictrl = SII_PRE;
	regs->dmctrl = 0;

	if (reset) {
		register int i;

		/*
		 * Assert SCSI bus reset for at least 25 Usec to clear the
		 * world. SII_DO_RST is self clearing.
		 * Delay 250 ms before doing any commands.
		 */
		regs->comm = SII_DO_RST;
		wbflush();
		DELAY(250000);

		/* rearbitrate synchronous offset */
		for (i = 0; i < SII_NCMD; i++)
			sc->sc_st[i].dmaReqAck = 0;
	}

	/*
	 * Clear any pending interrupts from the reset.
	 */
	regs->cstat = regs->cstat;
	regs->dstat = regs->dstat;
	/*
	 * Set up SII for arbitrated bus mode, SCSI parity checking,
	 * Reselect Enable, and Interrupt Enable.
	 */
	regs->csr = SII_HPM | SII_RSE | SII_PCE | SII_IE;
	wbflush();
}

/*
 * Start a SCSI command by sending the cmd data
 * to a SCSI controller via the SII.
 * Call the device done proceedure if it can't be started.
 * NOTE: we should be called with interrupts disabled.
 */
static void
sii_StartCmd(sc, target)
	register struct siisoftc *sc;	/* which SII to use */
	register int target;		/* which command to start */
{
	register SIIRegs *regs;	
	register ScsiCmd *scsicmd;
	register State *state;
	register u_int status;
	int error, retval;

	/* if another command is currently in progress, just wait */
	if (sc->sc_target >= 0)
		return;

	/* initialize state information for this command */
	scsicmd = sc->sc_cmd[target];
	state = &sc->sc_st[target];
	state->flags = FIRST_DMA;
	state->prevComm = 0;
	state->dmalen = 0;
	state->dmaCurPhase = -1;
	state->dmaPrevPhase = -1;
	state->dmaBufIndex = 0;
	state->cmd = scsicmd->cmd;
	state->cmdlen = scsicmd->cmdlen;
	if ((state->buflen = scsicmd->buflen) == 0) {
		state->dmaDataPhase = -1; /* illegal phase. shouldn't happen */
		state->buf = (char *)0;
	} else {
		state->dmaDataPhase =
			(scsicmd->flags & SCSICMD_DATA_TO_DEVICE) ?
			SII_DATA_OUT_PHASE : SII_DATA_IN_PHASE;
		state->buf = scsicmd->buf;
	}

#ifdef DEBUG
	if (sii_debug > 1) {
		printf("sii_StartCmd: %s target %d cmd 0x%x addr %p size %d dma %d\n",
			scsicmd->sd->sd_driver->d_name, target,
			scsicmd->cmd[0], scsicmd->buf, scsicmd->buflen,
			state->dmaDataPhase);
	}
	sii_debug_cmd = scsicmd->cmd[0];
	if (scsicmd->cmd[0] == SCSI_READ_EXT ||
	    scsicmd->cmd[0] == SCSI_WRITE_EXT) {
		sii_debug_bn = (scsicmd->cmd[2] << 24) |
			(scsicmd->cmd[3] << 16) |
			(scsicmd->cmd[4] << 8) |
			scsicmd->cmd[5];
		sii_debug_sz = (scsicmd->cmd[7] << 8) | scsicmd->cmd[8];
	} else {
		sii_debug_bn = 0;
		sii_debug_sz = 0;
	}
#endif

	/* try to select the target */
	regs = sc->sc_regs;

	/*
	 * Another device may have selected us; in which case,
	 * this command will be restarted later.
	 */
	if ((status = regs->dstat) & (SII_CI | SII_DI)) {
		sii_DoIntr(sc, status);
		return;
	}

	sc->sc_target = target;
#if 0
	/* seem to have problems with synchronous transfers */
	if (scsicmd->flags & SCSICMD_USE_SYNC) {
		printf("sii_StartCmd: doing extended msg\n"); /* XXX */
		/*
		 * Setup to send both the identify message and the synchronous
		 * data transfer request.
		 */
		sii_buf[0] = SCSI_DIS_REC_IDENTIFY;
		sii_buf[1] = SCSI_EXTENDED_MSG;
		sii_buf[2] = 3;		/* message length */
		sii_buf[3] = SCSI_SYNCHRONOUS_XFER;
		sii_buf[4] = 0;
		sii_buf[5] = 3;		/* maximum SII chip supports */

		state->dmaCurPhase = SII_MSG_OUT_PHASE,
		state->dmalen = 6;
		CopyToBuffer((u_short *)sii_buf,
			(volatile u_short *)SII_BUF_ADDR, 6);
		regs->slcsr = target;
		regs->dmctrl = state->dmaReqAck;
		regs->dmaddrl = (u_short)(SII_BUF_ADDR >> 1);
		regs->dmaddrh = (u_short)(SII_BUF_ADDR >> 17) & 03;
		regs->dmlotc = 6;
		regs->comm = SII_DMA | SII_INXFER | SII_SELECT | SII_ATN |
			SII_CON | SII_MSG_OUT_PHASE;
	} else
#endif
	{
		/* do a chained, select with ATN and programmed I/O command */
		regs->data = SCSI_DIS_REC_IDENTIFY;
		regs->slcsr = target;
		regs->dmctrl = state->dmaReqAck;
		regs->comm = SII_INXFER | SII_SELECT | SII_ATN | SII_CON |
			SII_MSG_OUT_PHASE;
	}
	wbflush();

	/*
	 * Wait for something to happen
	 * (should happen soon or we would use interrupts).
	 */
	SII_WAIT_UNTIL(status, regs->cstat, status & (SII_CI | SII_DI),
		SII_WAIT_COUNT/4, retval);

	/* check to see if we are connected OK */
	if ((status & (SII_RST | SII_SCH | SII_STATE_MSK)) ==
	    (SII_SCH | SII_CON)) {
		regs->cstat = status;
		wbflush();

#ifdef DEBUG
		sii_logp->target = target;
		sii_logp->cstat = status;
		sii_logp->dstat = 0;
		sii_logp->comm = regs->comm;
		sii_logp->msg = -1;
		sii_logp->rlen = state->buflen;
		sii_logp->dlen = state->dmalen;
		if (++sii_logp >= &sii_log[NLOG])
			sii_logp = sii_log;
#endif

		/* wait a short time for command phase */
		SII_WAIT_UNTIL(status, regs->dstat, status & SII_MIS,
			SII_WAIT_COUNT, retval);
#ifdef DEBUG
		if (sii_debug > 2)
			printf("sii_StartCmd: ds %x cnt %d\n", status, retval);
#endif
		if ((status & (SII_CI | SII_MIS | SII_PHASE_MSK)) !=
		    (SII_MIS | SII_CMD_PHASE)) {
			printf("sii_StartCmd: timeout cs %x ds %x cnt %d\n",
				regs->cstat, status, retval); /* XXX */
			/* process interrupt or continue until it happens */
			if (status & (SII_CI | SII_DI))
				sii_DoIntr(sc, status);
			return;
		}
		regs->dstat = SII_DNE;	/* clear Msg Out DMA done */

		/* send command data */
		CopyToBuffer((u_short *)state->cmd,
			(volatile u_short *)state->dmaAddr[0], state->cmdlen);
		sii_StartDMA(regs, state->dmaCurPhase = SII_CMD_PHASE,
			state->dmaAddr[0], state->dmalen = scsicmd->cmdlen);

		/* wait a little while for DMA to finish */
		SII_WAIT_UNTIL(status, regs->dstat, status & (SII_CI | SII_DI),
			SII_WAIT_COUNT, retval);
#ifdef DEBUG
		if (sii_debug > 2)
			printf("sii_StartCmd: ds %x, cnt %d\n", status, retval);
#endif
		if (status & (SII_CI | SII_DI))
			sii_DoIntr(sc, status);
#ifdef DEBUG
		if (sii_debug > 2)
			printf("sii_StartCmd: DONE ds %x\n", regs->dstat);
#endif
		return;
	}

	/*
	 * Another device may have selected us; in which case,
	 * this command will be restarted later.
	 */
	if (status & (SII_CI | SII_DI)) {
		sii_DoIntr(sc, regs->dstat);
		return;
	}

	/*
	 * Disconnect if selection command still in progress.
	 */
	if (status & SII_SIP) {
		error = ENXIO;	/* device didn't respond */
		regs->comm = SII_DISCON;
		wbflush();
		SII_WAIT_UNTIL(status, regs->cstat,
			!(status & (SII_CON | SII_SIP)),
			SII_WAIT_COUNT, retval);
	} else
		error = EBUSY;	/* couldn't get the bus */
#ifdef DEBUG
	if (sii_debug > 1)
		printf("sii_StartCmd: Couldn't select target %d error %d\n",
			target, error);
#endif
	sc->sc_target = -1;
	regs->cstat = 0xffff;
	regs->dstat = 0xffff;
	regs->comm = 0;
	wbflush();
	sii_CmdDone(sc, target, error);
}

/*
 * Process interrupt conditions.
 */
static void
sii_DoIntr(sc, dstat)
	register struct siisoftc *sc;
	register u_int dstat;
{
	register SIIRegs *regs = sc->sc_regs;
	register State *state;
	register u_int cstat;
	int i, msg;
	u_int comm;

again:
	comm = regs->comm;

#ifdef DEBUG
	if (sii_debug > 3)
		printf("sii_DoIntr: cs %x, ds %x cm %x ",
			regs->cstat, dstat, comm);
	sii_logp->target = sc->sc_target;
	sii_logp->cstat = regs->cstat;
	sii_logp->dstat = dstat;
	sii_logp->comm = comm;
	sii_logp->msg = -1;
	if (sc->sc_target >= 0) {
		sii_logp->rlen = sc->sc_st[sc->sc_target].buflen;
		sii_logp->dlen = sc->sc_st[sc->sc_target].dmalen;
	} else {
		sii_logp->rlen = 0;
		sii_logp->dlen = 0;
	}
	if (++sii_logp >= &sii_log[NLOG])
		sii_logp = sii_log;
#endif

	regs->dstat = dstat;	/* acknowledge everything */
	wbflush();

	if (dstat & SII_CI) {
		/* deglitch cstat register */
		msg = regs->cstat;
		while (msg != (cstat = regs->cstat))
			msg = cstat;
		regs->cstat = cstat;	/* acknowledge everything */
		wbflush();
#ifdef DEBUG
		if (sii_logp > sii_log)
			sii_logp[-1].cstat = cstat;
		else
			sii_log[NLOG - 1].cstat = cstat;
#endif

		/* check for a BUS RESET */
		if (cstat & SII_RST) {
			printf("%s: SCSI bus reset!!\n", sc->sc_dev.dv_xname);
			/* need to flush disconnected commands */
			for (i = 0; i < SII_NCMD; i++) {
				if (!sc->sc_cmd[i])
					continue;
				sii_CmdDone(sc, i, EIO);
			}
			/* rearbitrate synchronous offset */
			for (i = 0; i < SII_NCMD; i++)
				sc->sc_st[i].dmaReqAck = 0;
			sc->sc_target = -1;
			return;
		}

#ifdef notdef
		/*
		 * Check for a BUS ERROR.
		 * According to DEC, this feature doesn't really work
		 * and to just clear the bit if it's set.
		 */
		if (cstat & SII_BER) {
		}
#endif

		/* check for state change */
		if (cstat & SII_SCH) {
			sii_StateChg(sc, cstat);
			comm = regs->comm;
		}
	}

	/* check for DMA completion */
	if (dstat & SII_DNE) {
		u_short *dma;
		char *buf;

		/*
		 * There is a race condition with SII_SCH. There is a short
		 * window between the time a SII_SCH is seen after a disconnect
		 * and when the SII_SCH is cleared. A reselect can happen
		 * in this window and we will clear the SII_SCH without
		 * processing the reconnect.
		 */
		if (sc->sc_target < 0) {
			cstat = regs->cstat;
			printf("%s: target %d DNE?? dev %d,%d cs %x\n",
				sc->sc_dev.dv_xname, sc->sc_target,
				regs->slcsr, regs->destat,
				cstat); /* XXX */
			if (cstat & SII_DST) {
				sc->sc_target = regs->destat;
				state->prevComm = 0;
			} else
				panic("sc_target 1");
		}
		state = &sc->sc_st[sc->sc_target];
		/* check for a PARITY ERROR */
		if (dstat & SII_IPE) {
			state->flags |= PARITY_ERR;
			printf("%s: Parity error!!\n", sc->sc_dev.dv_xname);
			goto abort;
		}
		/* dmalen = amount left to transfer, i = amount transfered */
		i = state->dmalen;
		state->dmalen = 0;
		state->dmaCurPhase = -1;
#ifdef DEBUG
		if (sii_debug > 4) {
			printf("DNE: amt %d ", i);
			if (!(dstat & SII_TCZ))
				printf("no TCZ?? (%d) ", regs->dmlotc);
		} else if (!(dstat & SII_TCZ)) {
			printf("%s: device %d: no TCZ?? (%d)\n",
				sc->sc_dev.dv_xname, sc->sc_target, regs->dmlotc);
			sii_DumpLog(); /* XXX */
		}
#endif
		switch (comm & SII_PHASE_MSK) {
		case SII_CMD_PHASE:
			state->cmdlen -= i;
			break;

		case SII_DATA_IN_PHASE:
			/* check for more data for the same phase */
			dma = state->dmaAddr[state->dmaBufIndex];
			buf = state->buf;
			state->buf += i;
			state->buflen -= i;
			if (state->buflen > 0 && !(dstat & SII_MIS)) {
				int len;

				/* start reading next chunk */
				len = state->buflen;
				if (len > SII_MAX_DMA_XFER_LENGTH)
					len = SII_MAX_DMA_XFER_LENGTH;
				state->dmaBufIndex = !state->dmaBufIndex;
				sii_StartDMA(regs,
					state->dmaCurPhase = SII_DATA_IN_PHASE,
					state->dmaAddr[state->dmaBufIndex],
					state->dmaCnt = state->dmalen = len);
				dstat &= ~(SII_IBF | SII_TBE);
			}
			/* copy in the data */
			CopyFromBuffer((volatile u_short *)dma, buf, i);
			break;

		case SII_DATA_OUT_PHASE:
			state->dmaBufIndex = !state->dmaBufIndex;
			state->buf += i;
			state->buflen -= i;

			/* check for more data for the same phase */
			if (state->buflen <= 0 || (dstat & SII_MIS))
				break;

			/* start next chunk */
			i = state->buflen;
			if (i > SII_MAX_DMA_XFER_LENGTH) {
				sii_StartDMA(regs, state->dmaCurPhase =
					SII_DATA_OUT_PHASE,
					state->dmaAddr[state->dmaBufIndex],
					state->dmaCnt = state->dmalen =
					SII_MAX_DMA_XFER_LENGTH);
				/* prepare for next chunk */
				i -= SII_MAX_DMA_XFER_LENGTH;
				if (i > SII_MAX_DMA_XFER_LENGTH)
					i = SII_MAX_DMA_XFER_LENGTH;
				CopyToBuffer((u_short *)(state->buf +
					SII_MAX_DMA_XFER_LENGTH),
					(volatile u_short *)
					state->dmaAddr[!state->dmaBufIndex], i);
			} else {
				sii_StartDMA(regs, state->dmaCurPhase =
					SII_DATA_OUT_PHASE,
					state->dmaAddr[state->dmaBufIndex],
					state->dmaCnt = state->dmalen = i);
			}
			dstat &= ~(SII_IBF | SII_TBE);
		}
	}

	/* check for phase change or another MsgIn/Out */
	if (dstat & (SII_MIS | SII_IBF | SII_TBE)) {
		/*
		 * There is a race condition with SII_SCH. There is a short
		 * window between the time a SII_SCH is seen after a disconnect
		 * and when the SII_SCH is cleared. A reselect can happen
		 * in this window and we will clear the SII_SCH without
		 * processing the reconnect.
		 */
		if (sc->sc_target < 0) {
			cstat = regs->cstat;
			printf("%s: target %d MIS?? dev %d,%d cs %x ds %x\n",
				sc->sc_dev.dv_xname, sc->sc_target,
				regs->slcsr, regs->destat,
				cstat, dstat); /* XXX */
			if (cstat & SII_DST) {
				sc->sc_target = regs->destat;
				state->prevComm = 0;
			} else {
#ifdef DEBUG
				sii_DumpLog();
#endif
				panic("sc_target 2");
			}
		}
		state = &sc->sc_st[sc->sc_target];
		switch (dstat & SII_PHASE_MSK) {
		case SII_CMD_PHASE:
			if (state->dmaPrevPhase >= 0) {
				/* restart DMA after disconnect/reconnect */
				if (state->dmaPrevPhase != SII_CMD_PHASE) {
					printf("%s: device %d: dma reselect phase doesn't match\n",
						sc->sc_dev.dv_xname, sc->sc_target);
					goto abort;
				}
				state->dmaCurPhase = SII_CMD_PHASE;
				state->dmaPrevPhase = -1;
				regs->dmaddrl = state->dmaAddrL;
				regs->dmaddrh = state->dmaAddrH;
				regs->dmlotc = state->dmaCnt;
				if (state->dmaCnt & 1)
					regs->dmabyte = state->dmaByte;
				regs->comm = SII_DMA | SII_INXFER |
					(comm & SII_STATE_MSK) | SII_CMD_PHASE;
				wbflush();
#ifdef DEBUG
				if (sii_debug > 4)
					printf("Cmd dcnt %d dadr %x ",
						state->dmaCnt,
						(state->dmaAddrH << 16) |
							state->dmaAddrL);
#endif
			} else {
				/* send command data */
				i = state->cmdlen;
				if (i == 0) {
					printf("%s: device %d: cmd count exceeded\n",
						sc->sc_dev.dv_xname, sc->sc_target);
					goto abort;
				}
				CopyToBuffer((u_short *)state->cmd,
					(volatile u_short *)state->dmaAddr[0],
					i);
				sii_StartDMA(regs, state->dmaCurPhase =
					SII_CMD_PHASE, state->dmaAddr[0],
					state->dmaCnt = state->dmalen = i);
			}
			/* wait a short time for XFER complete */
			SII_WAIT_UNTIL(dstat, regs->dstat,
				dstat & (SII_CI | SII_DI), SII_WAIT_COUNT, i);
			if (dstat & (SII_CI | SII_DI)) {
#ifdef DEBUG
				if (sii_debug > 4)
					printf("cnt %d\n", i);
				else if (sii_debug > 0)
					printf("sii_DoIntr: cmd wait ds %x cnt %d\n",
						dstat, i);
#endif
				goto again;
			}
			break;

		case SII_DATA_IN_PHASE:
		case SII_DATA_OUT_PHASE:
			if (state->cmdlen > 0) {
				printf("%s: device %d: cmd %x: command data not all sent (%d) 1\n",
					sc->sc_dev.dv_xname, sc->sc_target,
					sc->sc_cmd[sc->sc_target]->cmd[0],
					state->cmdlen);
				state->cmdlen = 0;
#ifdef DEBUG
				sii_DumpLog();
#endif
			}
			if (state->dmaPrevPhase >= 0) {
				/* restart DMA after disconnect/reconnect */
				if (state->dmaPrevPhase !=
				    (dstat & SII_PHASE_MSK)) {
					printf("%s: device %d: dma reselect phase doesn't match\n",
						sc->sc_dev.dv_xname, sc->sc_target);
					goto abort;
				}
				state->dmaCurPhase = state->dmaPrevPhase;
				state->dmaPrevPhase = -1;
				regs->dmaddrl = state->dmaAddrL;
				regs->dmaddrh = state->dmaAddrH;
				regs->dmlotc = state->dmaCnt;
				if (state->dmaCnt & 1)
					regs->dmabyte = state->dmaByte;
				regs->comm = SII_DMA | SII_INXFER |
					(comm & SII_STATE_MSK) |
					state->dmaCurPhase;
				wbflush();
#ifdef DEBUG
				if (sii_debug > 4)
					printf("Data %d dcnt %d dadr %x ",
						state->dmaDataPhase,
						state->dmaCnt,
						(state->dmaAddrH << 16) |
							state->dmaAddrL);
#endif
				break;
			}
			if (state->dmaDataPhase != (dstat & SII_PHASE_MSK)) {
				printf("%s: device %d: cmd %x: dma phase doesn't match\n",
					sc->sc_dev.dv_xname, sc->sc_target,
					sc->sc_cmd[sc->sc_target]->cmd[0]);
				goto abort;
			}
#ifdef DEBUG
			if (sii_debug > 4) {
				printf("Data %d ", state->dmaDataPhase);
				if (sii_debug > 5)
					printf("\n");
			}
#endif
			i = state->buflen;
			if (i == 0) {
				printf("%s: device %d: data count exceeded\n",
					sc->sc_dev.dv_xname, sc->sc_target);
				goto abort;
			}
			if (i > SII_MAX_DMA_XFER_LENGTH)
				i = SII_MAX_DMA_XFER_LENGTH;
			if ((dstat & SII_PHASE_MSK) == SII_DATA_IN_PHASE) {
				sii_StartDMA(regs,
					state->dmaCurPhase = SII_DATA_IN_PHASE,
					state->dmaAddr[state->dmaBufIndex],
					state->dmaCnt = state->dmalen = i);
				break;
			}
			/* start first chunk */
			if (state->flags & FIRST_DMA) {
				state->flags &= ~FIRST_DMA;
				CopyToBuffer((u_short *)state->buf,
					(volatile u_short *)
					state->dmaAddr[state->dmaBufIndex], i);
			}
			sii_StartDMA(regs,
				state->dmaCurPhase = SII_DATA_OUT_PHASE,
				state->dmaAddr[state->dmaBufIndex],
				state->dmaCnt = state->dmalen = i);
			i = state->buflen - SII_MAX_DMA_XFER_LENGTH;
			if (i > 0) {
				/* prepare for next chunk */
				if (i > SII_MAX_DMA_XFER_LENGTH)
					i = SII_MAX_DMA_XFER_LENGTH;
				CopyToBuffer((u_short *)(state->buf +
					SII_MAX_DMA_XFER_LENGTH),
					(volatile u_short *)
					state->dmaAddr[!state->dmaBufIndex], i);
			}
			break;

		case SII_STATUS_PHASE:
			if (state->cmdlen > 0) {
				printf("%s: device %d: cmd %x: command data not all sent (%d) 2\n",
					sc->sc_dev.dv_xname, sc->sc_target,
					sc->sc_cmd[sc->sc_target]->cmd[0],
					state->cmdlen);
				state->cmdlen = 0;
#ifdef DEBUG
				sii_DumpLog();
#endif
			}

			/* read amount transfered if DMA didn't finish */
			if (state->dmalen > 0) {
				i = state->dmalen - regs->dmlotc;
				state->dmalen = 0;
				state->dmaCurPhase = -1;
				regs->dmlotc = 0;
				regs->comm = comm &
					(SII_STATE_MSK | SII_PHASE_MSK);
				wbflush();
				regs->dstat = SII_DNE;
				wbflush();
#ifdef DEBUG
				if (sii_debug > 4)
					printf("DMA amt %d ", i);
#endif
				switch (comm & SII_PHASE_MSK) {
				case SII_DATA_IN_PHASE:
					/* copy in the data */
					CopyFromBuffer((volatile u_short *)
					    state->dmaAddr[state->dmaBufIndex],
					    state->buf, i);

				case SII_CMD_PHASE:
				case SII_DATA_OUT_PHASE:
					state->buflen -= i;
				}
			}

			/* read a one byte status message */
			state->statusByte = msg =
				sii_GetByte(regs, SII_STATUS_PHASE, 1);
			if (msg < 0) {
				dstat = regs->dstat;
				goto again;
			}
#ifdef DEBUG
			if (sii_debug > 4)
				printf("Status %x ", msg);
			if (sii_logp > sii_log)
				sii_logp[-1].msg = msg;
			else
				sii_log[NLOG - 1].msg = msg;
#endif

			/* do a quick wait for COMMAND_COMPLETE */
			SII_WAIT_UNTIL(dstat, regs->dstat,
				dstat & (SII_CI | SII_DI), SII_WAIT_COUNT, i);
			if (dstat & (SII_CI | SII_DI)) {
#ifdef DEBUG
				if (sii_debug > 4)
					printf("cnt2 %d\n", i);
#endif
				goto again;
			}
			break;

		case SII_MSG_IN_PHASE:
			/*
			 * Save DMA state if DMA didn't finish.
			 * Be careful not to save state again after reconnect
			 * and see RESTORE_POINTER message.
			 * Note that the SII DMA address is not incremented
			 * as DMA proceeds.
			 */
			if (state->dmaCurPhase > 0) {
				/* save dma registers */
				state->dmaPrevPhase = state->dmaCurPhase;
				state->dmaCurPhase = -1;
				if (dstat & SII_OBB)
					state->dmaByte = regs->dmabyte;
				i = regs->dmlotc;
				if (i != 0)
					i = state->dmaCnt - i;
				/* note: no carry from dmaddrl to dmaddrh */
				state->dmaAddrL = regs->dmaddrl + i;
				state->dmaAddrH = regs->dmaddrh;
				state->dmaCnt = regs->dmlotc;
				if (state->dmaCnt == 0)
					state->dmaCnt = SII_MAX_DMA_XFER_LENGTH;
				regs->comm = comm &
					(SII_STATE_MSK | SII_PHASE_MSK);
				wbflush();
				regs->dstat = SII_DNE;
				wbflush();
#ifdef DEBUG
				if (sii_debug > 4) {
					printf("SavP dcnt %d dadr %x ",
						state->dmaCnt,
						(state->dmaAddrH << 16) |
						state->dmaAddrL);
					if (((dstat & SII_OBB) != 0) ^
					    (state->dmaCnt & 1))
						printf("OBB??? ");
				} else if (sii_debug > 0) {
					if (((dstat & SII_OBB) != 0) ^
					    (state->dmaCnt & 1)) {
						printf("sii_DoIntr: OBB??? ds %x cnt %d\n",
							dstat, state->dmaCnt);
						sii_DumpLog();
					}
				}
#endif
			}

			/* read a one byte message */
			msg = sii_GetByte(regs, SII_MSG_IN_PHASE, 0);
			if (msg < 0) {
				dstat = regs->dstat;
				goto again;
			}
#ifdef DEBUG
			if (sii_debug > 4)
				printf("MsgIn %x ", msg);
			if (sii_logp > sii_log)
				sii_logp[-1].msg = msg;
			else
				sii_log[NLOG - 1].msg = msg;
#endif

			/* process message */
			switch (msg) {
			case SCSI_COMMAND_COMPLETE:
				/* acknowledge last byte */
				regs->comm = SII_INXFER | SII_MSG_IN_PHASE |
					(comm & SII_STATE_MSK);
				SII_WAIT_UNTIL(dstat, regs->dstat,
					dstat & SII_DNE, SII_WAIT_COUNT, i);
				regs->dstat = SII_DNE;
				wbflush();
				msg = sc->sc_target;
				sc->sc_target = -1;
				/*
				 * Wait a short time for disconnect.
				 * Don't be fooled if SII_BER happens first.
				 * Note: a reselect may happen here.
				 */
				SII_WAIT_UNTIL(cstat, regs->cstat,
					cstat & (SII_RST | SII_SCH),
					SII_WAIT_COUNT, i);
				if ((cstat & (SII_RST | SII_SCH |
				    SII_STATE_MSK)) == SII_SCH) {
					regs->cstat = SII_SCH | SII_BER;
					regs->comm = 0;
					wbflush();
					/*
					 * Double check that we didn't miss a
					 * state change between seeing it and
					 * clearing the SII_SCH bit.
					 */
					i = regs->cstat;
					if (!(i & SII_SCH) &&
					    (i & SII_STATE_MSK) !=
					    (cstat & SII_STATE_MSK))
						sii_StateChg(sc, i);
				}
#ifdef DEBUG
				if (sii_debug > 4)
					printf("cs %x\n", cstat);
#endif
				sii_CmdDone(sc, msg, 0);
				break;

			case SCSI_EXTENDED_MSG:
				/* acknowledge last byte */
				regs->comm = SII_INXFER | SII_MSG_IN_PHASE |
					(comm & SII_STATE_MSK);
				SII_WAIT_UNTIL(dstat, regs->dstat,
					dstat & SII_DNE, SII_WAIT_COUNT, i);
				regs->dstat = SII_DNE;
				wbflush();
				/* read the message length */
				msg = sii_GetByte(regs, SII_MSG_IN_PHASE, 1);
				if (msg < 0) {
					dstat = regs->dstat;
					goto again;
				}
				sii_buf[1] = msg;	/* message length */
				if (msg == 0)
					msg = 256;
				/*
				 * We read and acknowlege all the bytes
				 * except the last so we can assert ATN
				 * if needed before acknowledging the last.
				 */
				for (i = 0; i < msg; i++) {
					dstat = sii_GetByte(regs,
						SII_MSG_IN_PHASE, i < msg - 1);
					if ((int)dstat < 0) {
						dstat = regs->dstat;
						goto again;
					}
					sii_buf[i + 2] = dstat;
				}

				switch (sii_buf[2]) {
				case SCSI_MODIFY_DATA_PTR:
					/* acknowledge last byte */
					regs->comm = SII_INXFER |
						SII_MSG_IN_PHASE |
						(comm & SII_STATE_MSK);
					SII_WAIT_UNTIL(dstat, regs->dstat,
						dstat & SII_DNE,
						SII_WAIT_COUNT, i);
					regs->dstat = SII_DNE;
					wbflush();
					i = (sii_buf[3] << 24) |
						(sii_buf[4] << 16) |
						(sii_buf[5] << 8) |
						sii_buf[6];
					if (state->dmaPrevPhase >= 0) {
						state->dmaAddrL += i;
						state->dmaCnt -= i;
					}
					break;

				case SCSI_SYNCHRONOUS_XFER:
					/*
					 * Acknowledge last byte and
					 * signal a request for MSG_OUT.
					 */
					regs->comm = SII_INXFER | SII_ATN |
						SII_MSG_IN_PHASE |
						(comm & SII_STATE_MSK);
					SII_WAIT_UNTIL(dstat, regs->dstat,
						dstat & SII_DNE,
						SII_WAIT_COUNT, i);
					regs->dstat = SII_DNE;
					wbflush();
					sii_DoSync(regs, state);
					break;

				default:
				reject:
					/*
					 * Acknowledge last byte and
					 * signal a request for MSG_OUT.
					 */
					regs->comm = SII_INXFER | SII_ATN |
						SII_MSG_IN_PHASE |
						(comm & SII_STATE_MSK);
					SII_WAIT_UNTIL(dstat, regs->dstat,
						dstat & SII_DNE,
						SII_WAIT_COUNT, i);
					regs->dstat = SII_DNE;
					wbflush();

					/* wait for MSG_OUT phase */
					SII_WAIT_UNTIL(dstat, regs->dstat,
						dstat & SII_TBE,
						SII_WAIT_COUNT, i);

					/* send a reject message */
					regs->data = SCSI_MESSAGE_REJECT;
					regs->comm = SII_INXFER |
						(regs->cstat & SII_STATE_MSK) |
						SII_MSG_OUT_PHASE;
					SII_WAIT_UNTIL(dstat, regs->dstat,
						dstat & SII_DNE,
						SII_WAIT_COUNT, i);
					regs->dstat = SII_DNE;
					wbflush();
				}
				break;

			case SCSI_SAVE_DATA_POINTER:
			case SCSI_RESTORE_POINTERS:
				/* acknowledge last byte */
				regs->comm = SII_INXFER | SII_MSG_IN_PHASE |
					(comm & SII_STATE_MSK);
				SII_WAIT_UNTIL(dstat, regs->dstat,
					dstat & SII_DNE, SII_WAIT_COUNT, i);
				regs->dstat = SII_DNE;
				wbflush();
				/* wait a short time for another msg */
				SII_WAIT_UNTIL(dstat, regs->dstat,
					dstat & (SII_CI | SII_DI),
					SII_WAIT_COUNT, i);
				if (dstat & (SII_CI | SII_DI)) {
#ifdef DEBUG
					if (sii_debug > 4)
						printf("cnt %d\n", i);
#endif
					goto again;
				}
				break;

			case SCSI_DISCONNECT:
				/* acknowledge last byte */
				regs->comm = SII_INXFER | SII_MSG_IN_PHASE |
					(comm & SII_STATE_MSK);
				SII_WAIT_UNTIL(dstat, regs->dstat,
					dstat & SII_DNE, SII_WAIT_COUNT, i);
				regs->dstat = SII_DNE;
				wbflush();
				state->prevComm = comm;
#ifdef DEBUG
				if (sii_debug > 4)
					printf("disconn %d ", sc->sc_target);
#endif
				/*
				 * Wait a short time for disconnect.
				 * Don't be fooled if SII_BER happens first.
				 * Note: a reselect may happen here.
				 */
				SII_WAIT_UNTIL(cstat, regs->cstat,
					cstat & (SII_RST | SII_SCH),
					SII_WAIT_COUNT, i);
				if ((cstat & (SII_RST | SII_SCH |
				    SII_STATE_MSK)) != SII_SCH) {
#ifdef DEBUG
					if (sii_debug > 4)
						printf("cnt %d\n", i);
#endif
					dstat = regs->dstat;
					goto again;
				}
				regs->cstat = SII_SCH | SII_BER;
				regs->comm = 0;
				wbflush();
				sc->sc_target = -1;
				/*
				 * Double check that we didn't miss a state
				 * change between seeing it and clearing
				 * the SII_SCH bit.
				 */
				i = regs->cstat;
				if (!(i & SII_SCH) && (i & SII_STATE_MSK) !=
				    (cstat & SII_STATE_MSK))
					sii_StateChg(sc, i);
				break;

			case SCSI_MESSAGE_REJECT:
				/* acknowledge last byte */
				regs->comm = SII_INXFER | SII_MSG_IN_PHASE |
					(comm & SII_STATE_MSK);
				SII_WAIT_UNTIL(dstat, regs->dstat,
					dstat & SII_DNE, SII_WAIT_COUNT, i);
				regs->dstat = SII_DNE;
				wbflush();
				printf("%s: device %d: message reject.\n",
					sc->sc_dev.dv_xname, sc->sc_target);
				break;

			default:
				if (!(msg & SCSI_IDENTIFY)) {
					printf("%s: device %d: couldn't handle message 0x%x... rejecting.\n",
						sc->sc_dev.dv_xname, sc->sc_target,
						msg);
#ifdef DEBUG
					sii_DumpLog();
#endif
					goto reject;
				}
				/* acknowledge last byte */
				regs->comm = SII_INXFER | SII_MSG_IN_PHASE |
					(comm & SII_STATE_MSK);
				SII_WAIT_UNTIL(dstat, regs->dstat,
					dstat & SII_DNE, SII_WAIT_COUNT, i);
				regs->dstat = SII_DNE;
				wbflush();
				/* may want to check LUN some day */
				/* wait a short time for another msg */
				SII_WAIT_UNTIL(dstat, regs->dstat,
					dstat & (SII_CI | SII_DI),
					SII_WAIT_COUNT, i);
				if (dstat & (SII_CI | SII_DI)) {
#ifdef DEBUG
					if (sii_debug > 4)
						printf("cnt %d\n", i);
#endif
					goto again;
				}
			}
			break;

		case SII_MSG_OUT_PHASE:
#ifdef DEBUG
			if (sii_debug > 4)
				printf("MsgOut\n");
#endif
			printf("MsgOut %x\n", state->flags); /* XXX */

			/*
			 * Check for parity error.
			 * Hardware will automatically set ATN
			 * to request the device for a MSG_OUT phase.
			 */
			if (state->flags & PARITY_ERR) {
				state->flags &= ~PARITY_ERR;
				regs->data = SCSI_MESSAGE_PARITY_ERROR;
			} else
				regs->data = SCSI_NO_OP;
			regs->comm = SII_INXFER | (comm & SII_STATE_MSK) |
				SII_MSG_OUT_PHASE;
			wbflush();

			/* wait a short time for XFER complete */
			SII_WAIT_UNTIL(dstat, regs->dstat, dstat & SII_DNE,
				SII_WAIT_COUNT, i);
#ifdef DEBUG
			if (sii_debug > 4)
				printf("ds %x i %d\n", dstat, i);
#endif
			/* just clear the DNE bit and check errors later */
			if (dstat & SII_DNE) {
				regs->dstat = SII_DNE;
				wbflush();
			}
			break;

		default:
			printf("%s: Couldn't handle phase %d... ignoring.\n",
				   sc->sc_dev.dv_xname, dstat & SII_PHASE_MSK);
		}
	}

#ifdef DEBUG
	if (sii_debug > 3)
		printf("\n");
#endif
	/*
	 * Check to make sure we won't be interrupted again.
	 * Deglitch dstat register.
	 */
	msg = regs->dstat;
	while (msg != (dstat = regs->dstat))
		msg = dstat;
	if (dstat & (SII_CI | SII_DI))
		goto again;

	if (sc->sc_target < 0) {
		/* look for another device that is ready */
		for (i = 0; i < SII_NCMD; i++) {
			/* don't restart a disconnected command */
			if (!sc->sc_cmd[i] || sc->sc_st[i].prevComm)
				continue;
			sii_StartCmd(sc, i);
			break;
		}
	}
	return;

abort:
	/* jump here to abort the current command */
	printf("%s: device %d: current command terminated\n",
		sc->sc_dev.dv_xname, sc->sc_target);
#ifdef DEBUG
	sii_DumpLog();
#endif

	if ((cstat = regs->cstat) & SII_CON) {
		/* try to send an abort msg for awhile */
		regs->dstat = SII_DNE;
		regs->data = SCSI_ABORT;
		regs->comm = SII_INXFER | SII_ATN | (cstat & SII_STATE_MSK) |
			SII_MSG_OUT_PHASE;
		wbflush();
		SII_WAIT_UNTIL(dstat, regs->dstat,
			(dstat & (SII_DNE | SII_PHASE_MSK)) ==
			(SII_DNE | SII_MSG_OUT_PHASE),
			2 * SII_WAIT_COUNT, i);
#ifdef DEBUG
		if (sii_debug > 0)
			printf("Abort: cs %x ds %x i %d\n", cstat, dstat, i);
#endif
		if ((dstat & (SII_DNE | SII_PHASE_MSK)) ==
		    (SII_DNE | SII_MSG_OUT_PHASE)) {
			/* disconnect if command in progress */
			regs->comm = SII_DISCON;
			wbflush();
			SII_WAIT_UNTIL(cstat, regs->cstat,
				!(cstat & SII_CON), SII_WAIT_COUNT, i);
		}
	} else {
#ifdef DEBUG
		if (sii_debug > 0)
			printf("Abort: cs %x\n", cstat);
#endif
	}
	regs->cstat = 0xffff;
	regs->dstat = 0xffff;
	regs->comm = 0;
	wbflush();

	i = sc->sc_target;
	sc->sc_target = -1;
	sii_CmdDone(sc, i, EIO);
#ifdef DEBUG
	if (sii_debug > 4)
		printf("sii_DoIntr: after CmdDone target %d\n", sc->sc_target);
#endif
}

static void
sii_StateChg(sc, cstat)
	register struct siisoftc *sc;
	register u_int cstat;
{
	register SIIRegs *regs = sc->sc_regs;
	register State *state;
	register int i;

#ifdef DEBUG
	if (sii_debug > 4)
		printf("SCH: ");
#endif

	switch (cstat & SII_STATE_MSK) {
	case 0:
		/* disconnect */
		i = sc->sc_target;
		sc->sc_target = -1;
#ifdef DEBUG
		if (sii_debug > 4)
			printf("disconn %d ", i);
#endif
		if (i >= 0 && !sc->sc_st[i].prevComm) {
			printf("%s: device %d: spurrious disconnect (%d)\n",
				sc->sc_dev.dv_xname, i, regs->slcsr);
			sc->sc_st[i].prevComm = 0;
		}
		break;

	case SII_CON:
		/* connected as initiator */
		i = regs->slcsr;
		if (sc->sc_target == i)
			break;
		printf("%s: device %d: connect to device %d??\n",
			sc->sc_dev.dv_xname, sc->sc_target, i);
		sc->sc_target = i;
		break;

	case SII_DST:
		/*
		 * Wait for CON to become valid,
		 * chip is slow sometimes.
		 */
		SII_WAIT_UNTIL(cstat, regs->cstat,
			cstat & SII_CON, SII_WAIT_COUNT, i);
		if (!(cstat & SII_CON))
			panic("sii resel");
		/* FALLTHROUGH */

	case SII_CON | SII_DST:
		/*
		 * Its a reselection. Save the ID and wait for
		 * interrupts to tell us what to do next
		 * (should be MSG_IN of IDENTIFY).
		 * NOTE: sc_target may be >= 0 if we were in
		 * the process of trying to start a command
		 * and were reselected before the select
		 * command finished.
		 */
		sc->sc_target = i = regs->destat;
		state = &sc->sc_st[i];
		regs->comm = SII_CON | SII_DST | SII_MSG_IN_PHASE;
		regs->dmctrl = state->dmaReqAck;
		wbflush();
		if (!state->prevComm) {
			printf("%s: device %d: spurious reselection\n",
				sc->sc_dev.dv_xname, i);
			break;
		}
		state->prevComm = 0;
#ifdef DEBUG
		if (sii_debug > 4)
			printf("resel %d ", sc->sc_target);
#endif
		break;

#ifdef notyet
	case SII_DST | SII_TGT:
	case SII_CON | SII_DST | SII_TGT:
		/* connected as target */
		printf("%s: Selected by device %d as target!!\n",
			sc->sc_dev.dv_xname, regs->destat);
		regs->comm = SII_DISCON;
		wbflush();
		SII_WAIT_UNTIL(!(regs->cstat & SII_CON),
			SII_WAIT_COUNT, i);
		regs->cstat = 0xffff;
		regs->dstat = 0xffff;
		regs->comm = 0;
		break;
#endif

	default:
		printf("%s: Unknown state change (cs %x)!!\n",
			sc->sc_dev.dv_xname, cstat);
#ifdef DEBUG
		sii_DumpLog();
#endif
	}
}

/*
 * Read one byte of data.
 * If 'ack' is true, acknowledge the byte.
 */
static int
sii_GetByte(regs, phase, ack)
	register SIIRegs *regs;
	int phase, ack;
{
	register u_int dstat;
	register u_int state;
	register int i;
	register int data;

	dstat = regs->dstat;
	state = regs->cstat & SII_STATE_MSK;
	if (!(dstat & SII_IBF) || (dstat & SII_MIS)) {
		regs->comm = state | phase;
		wbflush();
		/* wait a short time for IBF */
		SII_WAIT_UNTIL(dstat, regs->dstat, dstat & SII_IBF,
			SII_WAIT_COUNT, i);
#ifdef DEBUG
		if (!(dstat & SII_IBF))
			printf("status no IBF\n");
#endif
	}
	if (dstat & SII_DNE) { /* XXX */
		printf("sii_GetByte: DNE set 5\n");
#ifdef DEBUG
		sii_DumpLog();
#endif
		regs->dstat = SII_DNE;
	}
	data = regs->data;
	/* check for parity error */
	if (dstat & SII_IPE) {
#ifdef DEBUG
		if (sii_debug > 4)
			printf("cnt0 %d\n", i);
#endif
		printf("sii_GetByte: data %x ?? ds %x cm %x i %d\n",
			data, dstat, regs->comm, i); /* XXX */
		data = -1;
		ack = 1;
	}

	if (ack) {
		regs->comm = SII_INXFER | state | phase;
		wbflush();

		/* wait a short time for XFER complete */
		SII_WAIT_UNTIL(dstat, regs->dstat, dstat & SII_DNE,
			SII_WAIT_COUNT, i);

		/* clear the DNE */
		if (dstat & SII_DNE) {
			regs->dstat = SII_DNE;
			wbflush();
		}
	}

	return (data);
}

/*
 * Exchange messages to initiate synchronous data transfers.
 */
static void
sii_DoSync(regs, state)
	register SIIRegs *regs;
	register State *state;
{
	register u_int dstat, comm;
	register int i, j;
	u_int len;

#ifdef DEBUG
	if (sii_debug)
		printf("sii_DoSync: len %d per %d req/ack %d\n",
			sii_buf[1], sii_buf[3], sii_buf[4]);
#endif

	/* SII chip can only handle a minimum transfer period of ??? */
	if (sii_buf[3] < 64)
		sii_buf[3] = 64;
	/* SII chip can only handle a maximum REQ/ACK offset of 3 */
	len = sii_buf[4];
	if (len > 3)
		len = 3;

	sii_buf[0] = SCSI_EXTENDED_MSG;
	sii_buf[1] = 3;		/* message length */
	sii_buf[2] = SCSI_SYNCHRONOUS_XFER;
	sii_buf[4] = len;
#if 1
	comm = SII_INXFER | SII_ATN | SII_MSG_OUT_PHASE |
		(regs->cstat & SII_STATE_MSK);
	regs->comm = comm & ~SII_INXFER;
	for (j = 0; j < 5; j++) {
		/* wait for target to request the next byte */
		SII_WAIT_UNTIL(dstat, regs->dstat, dstat & SII_TBE,
			SII_WAIT_COUNT, i);
		if (!(dstat & SII_TBE) ||
		    (dstat & SII_PHASE_MSK) != SII_MSG_OUT_PHASE) {
			printf("sii_DoSync: TBE? ds %x cm %x i %d\n",
				dstat, comm, i); /* XXX */
			return;
		}

		/* the last message byte should have ATN off */
		if (j == 4)
			comm &= ~SII_ATN;

		regs->data = sii_buf[j];
		regs->comm = comm;
		wbflush();

		/* wait a short time for XFER complete */
		SII_WAIT_UNTIL(dstat, regs->dstat, dstat & SII_DNE,
			SII_WAIT_COUNT, i);

		if (!(dstat & SII_DNE)) {
			printf("sii_DoSync: DNE? ds %x cm %x i %d\n",
				dstat, comm, i); /* XXX */
			return;
		}

		/* clear the DNE, other errors handled later */
		regs->dstat = SII_DNE;
		wbflush();
	}
#else
	CopyToBuffer((u_short *)sii_buf, (volatile u_short *)SII_BUF_ADDR, 5);
	printf("sii_DoSync: %x %x %x ds %x\n",
		((volatile u_short *)SII_BUF_ADDR)[0],
		((volatile u_short *)SII_BUF_ADDR)[2],
		((volatile u_short *)SII_BUF_ADDR)[4],
		regs->dstat); /* XXX */
	regs->dmaddrl = (u_short)(SII_BUF_ADDR >> 1);
	regs->dmaddrh = (u_short)(SII_BUF_ADDR >> 17) & 03;
	regs->dmlotc = 5;
	regs->comm = SII_DMA | SII_INXFER | SII_ATN |
		(regs->cstat & SII_STATE_MSK) | SII_MSG_OUT_PHASE;
	wbflush();

	/* wait a short time for XFER complete */
	SII_WAIT_UNTIL(dstat, regs->dstat,
		(dstat & (SII_DNE | SII_TCZ)) == (SII_DNE | SII_TCZ),
		SII_WAIT_COUNT, i);

	if ((dstat & (SII_DNE | SII_TCZ)) != (SII_DNE | SII_TCZ)) {
		printf("sii_DoSync: ds %x cm %x i %d lotc %d\n",
			dstat, regs->comm, i, regs->dmlotc); /* XXX */
		sii_DumpLog(); /* XXX */
		return;
	}
	/* clear the DNE, other errors handled later */
	regs->dstat = SII_DNE;
	wbflush();
#endif

#if 0
	SII_WAIT_UNTIL(dstat, regs->dstat, dstat & (SII_CI | SII_DI),
		SII_WAIT_COUNT, i);
	printf("sii_DoSync: ds %x cm %x i %d lotc %d\n",
		dstat, regs->comm, i, regs->dmlotc); /* XXX */
#endif

	state->dmaReqAck = len;
}

/*
 * Issue the sequence of commands to the controller to start DMA.
 * NOTE: the data buffer should be word-aligned for DMA out.
 */
static void
sii_StartDMA(regs, phase, dmaAddr, size)
	register SIIRegs *regs;	/* which SII to use */
	int phase;		/* phase to send/receive data */
	u_short *dmaAddr;	/* DMA buffer address */
	int size;		/* # of bytes to transfer */
{

	if (regs->dstat & SII_DNE) { /* XXX */
		regs->dstat = SII_DNE;
		printf("sii_StartDMA: DNE set\n");
#ifdef DEBUG
		sii_DumpLog();
#endif
	}
	regs->dmaddrl = ((u_long)dmaAddr >> 1);
	regs->dmaddrh = ((u_long)dmaAddr >> 17) & 03;
	regs->dmlotc = size;
	regs->comm = SII_DMA | SII_INXFER | (regs->cstat & SII_STATE_MSK) |
		phase;
	wbflush();

#ifdef DEBUG
	if (sii_debug > 5) {
		printf("sii_StartDMA: cs 0x%x, ds 0x%x, cm 0x%x, size %d\n",
			regs->cstat, regs->dstat, regs->comm, size);
	}
#endif
}

/*
 * Call the device driver's 'done' routine to let it know the command is done.
 * The 'done' routine may try to start another command.
 * To be fair, we should start pending commands for other devices
 * before allowing the same device to start another command.
 */
static void
sii_CmdDone(sc, target, error)
	register struct siisoftc *sc;	/* which SII to use */
	int target;			/* which device is done */
	int error;			/* error code if any errors */
{
	register ScsiCmd *scsicmd;
	register int i;

	scsicmd = sc->sc_cmd[target];
#ifdef DIAGNOSTIC
	if (target < 0 || !scsicmd)
		panic("sii_CmdDone");
#endif
	sc->sc_cmd[target] = (ScsiCmd *)0;
#ifdef DEBUG
	if (sii_debug > 1) {
		printf("sii_CmdDone: %s target %d cmd %x err %d resid %d\n",
			scsicmd->sd->sd_driver->d_name, target,
			scsicmd->cmd[0], error, sc->sc_st[target].buflen);
	}
#endif

	/* look for another device that is ready */
	for (i = 0; i < SII_NCMD; i++) {
		/* don't restart a disconnected command */
		if (!sc->sc_cmd[i] || sc->sc_st[i].prevComm)
			continue;
		sii_StartCmd(sc, i);
		break;
	}

	(*scsicmd->sd->sd_driver->d_done)(scsicmd->unit, error,
		sc->sc_st[target].buflen, sc->sc_st[target].statusByte);
}

#ifdef DEBUG
void
sii_DumpLog()
{
	register struct sii_log *lp;

	printf("sii: cmd %x bn %d cnt %d\n", sii_debug_cmd, sii_debug_bn,
		sii_debug_sz);
	lp = sii_logp;
	do {
		printf("target %d cs %x ds %x cm %x msg %x rlen %x dlen %x\n",
			lp->target, lp->cstat, lp->dstat, lp->comm, lp->msg,
			lp->rlen, lp->dlen);
		if (++lp >= &sii_log[NLOG])
			lp = sii_log;
	} while (lp != sii_logp);
}
#endif
#endif

