/*	$OpenBSD: cn30xxipd.c,v 1.11 2017/11/05 04:57:28 visa Exp $	*/

/*
 * Copyright (c) 2007 Internet Initiative Japan, Inc.
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

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>

#include <machine/octeonvar.h>

#include <octeon/dev/cn30xxciureg.h>
#include <octeon/dev/cn30xxfpavar.h>
#include <octeon/dev/cn30xxgmxreg.h>
#include <octeon/dev/cn30xxpipreg.h>
#include <octeon/dev/cn30xxipdreg.h>
#include <octeon/dev/cn30xxipdvar.h>

void
cn30xxipd_init(struct cn30xxipd_attach_args *aa,
    struct cn30xxipd_softc **rsc)
{
	struct cn30xxipd_softc *sc;
	int status;

	sc = malloc(sizeof(*sc), M_DEVBUF, M_WAITOK | M_ZERO);
	if (sc == NULL)
		panic("can't allocate memory: %s", __func__);

	sc->sc_port = aa->aa_port;
	sc->sc_regt = aa->aa_regt;
	sc->sc_first_mbuff_skip = aa->aa_first_mbuff_skip;
	sc->sc_not_first_mbuff_skip = aa->aa_not_first_mbuff_skip;

	status = bus_space_map(sc->sc_regt, IPD_BASE, IPD_SIZE, 0,
	    &sc->sc_regh);
	if (status != 0)
		panic("can't map %s space", "ipd register");

	*rsc = sc;
}

#define	_IPD_RD8(sc, off) \
	bus_space_read_8((sc)->sc_regt, (sc)->sc_regh, (off))
#define	_IPD_WR8(sc, off, v) \
	bus_space_write_8((sc)->sc_regt, (sc)->sc_regh, (off), (v))

int
cn30xxipd_enable(struct cn30xxipd_softc *sc)
{
	uint64_t ctl_status;

	ctl_status = _IPD_RD8(sc, IPD_CTL_STATUS_OFFSET);
	SET(ctl_status, IPD_CTL_STATUS_IPD_EN);
	_IPD_WR8(sc, IPD_CTL_STATUS_OFFSET, ctl_status);

	return 0;
}

int
cn30xxipd_config(struct cn30xxipd_softc *sc)
{
	uint64_t first_mbuff_skip;
	uint64_t not_first_mbuff_skip;
	uint64_t packet_mbuff_size;
	uint64_t first_next_ptr_back;
	uint64_t second_next_ptr_back;
	uint64_t sqe_fpa_queue;
	uint64_t ctl_status;

	/* XXX */
	first_mbuff_skip = 0;
	SET(first_mbuff_skip, (sc->sc_first_mbuff_skip / 8) & IPD_1ST_MBUFF_SKIP_SZ);
	_IPD_WR8(sc, IPD_1ST_MBUFF_SKIP_OFFSET, first_mbuff_skip);
	/* XXX */

	/* XXX */
	not_first_mbuff_skip = 0;
	SET(not_first_mbuff_skip, (sc->sc_not_first_mbuff_skip / 8) &
	    IPD_NOT_1ST_MBUFF_SKIP_SZ);
	_IPD_WR8(sc, IPD_NOT_1ST_MBUFF_SKIP_OFFSET, not_first_mbuff_skip);
	/* XXX */

	packet_mbuff_size = 0;
	SET(packet_mbuff_size, (OCTEON_POOL_SIZE_PKT / 8) &
	    IPD_PACKET_MBUFF_SIZE_MB_SIZE);
	_IPD_WR8(sc, IPD_PACKET_MBUFF_SIZE_OFFSET, packet_mbuff_size);

	first_next_ptr_back = 0;
	SET(first_next_ptr_back, (sc->sc_first_mbuff_skip / CACHELINESIZE)
	    & IPD_1ST_NEXT_PTR_BACK_BACK);
	_IPD_WR8(sc, IPD_1ST_NEXT_PTR_BACK_OFFSET, first_next_ptr_back);

	second_next_ptr_back = 0;
	SET(second_next_ptr_back, (sc->sc_not_first_mbuff_skip / CACHELINESIZE)
	    & IPD_2ND_NEXT_PTR_BACK_BACK);
	_IPD_WR8(sc, IPD_2ND_NEXT_PTR_BACK_OFFSET, second_next_ptr_back);

	sqe_fpa_queue = 0;
	SET(sqe_fpa_queue, OCTEON_POOL_NO_WQE & IPD_WQE_FPA_QUEUE_WQE_QUE);
	_IPD_WR8(sc, IPD_WQE_FPA_QUEUE_OFFSET, sqe_fpa_queue);

	ctl_status = _IPD_RD8(sc, IPD_CTL_STATUS_OFFSET);
	CLR(ctl_status, IPD_CTL_STATUS_OPC_MODE);
	SET(ctl_status, IPD_CTL_STATUS_OPC_MODE_ALL);
	SET(ctl_status, IPD_CTL_STATUS_PBP_EN);

	_IPD_WR8(sc, IPD_CTL_STATUS_OFFSET, ctl_status);

	return 0;
}

/*
 * octeon work queue entry offload
 * L3 error & L4 error
 */
void
cn30xxipd_offload(uint64_t word2, uint16_t *rcflags)
{
	int cflags;

	/* Skip if the packet is non-IP. */
	if (ISSET(word2, PIP_WQE_WORD2_IP_NI))
		return;

	cflags = 0;

	/* Check IP checksum status. */
	if (!ISSET(word2, PIP_WQE_WORD2_IP_V6) &&
	    !ISSET(word2, PIP_WQE_WORD2_IP_IE))
		SET(cflags, M_IPV4_CSUM_IN_OK);

	/* Check TCP/UDP checksum status. Skip if the packet is a fragment. */
	if (ISSET(word2, PIP_WQE_WORD2_IP_TU) &&
	    !ISSET(word2, PIP_WQE_WORD2_IP_FR) &&
	    !ISSET(word2, PIP_WQE_WORD2_IP_LE))
		SET(cflags, M_TCP_CSUM_IN_OK | M_UDP_CSUM_IN_OK);

	*rcflags = cflags;
}

void
cn30xxipd_sub_port_fcs(struct cn30xxipd_softc *sc, int enable)
{
	uint64_t sub_port_fcs;

	sub_port_fcs = _IPD_RD8(sc, IPD_SUB_PORT_FCS_OFFSET);
	if (enable == 0)
		CLR(sub_port_fcs, 1 << sc->sc_port);
	else
		SET(sub_port_fcs, 1 << sc->sc_port);
	_IPD_WR8(sc, IPD_SUB_PORT_FCS_OFFSET, sub_port_fcs);
}
