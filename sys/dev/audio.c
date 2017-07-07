/*	$OpenBSD: audio.c,v 1.165 2017/06/26 07:02:16 ratchov Exp $	*/
/*
 * Copyright (c) 2015 Alexandre Ratchov <alex@caoua.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <sys/param.h>
#include <sys/fcntl.h>
#include <sys/systm.h>
#include <sys/ioctl.h>
#include <sys/conf.h>
#include <sys/poll.h>
#include <sys/kernel.h>
#include <sys/task.h>
#include <sys/vnode.h>
#include <sys/malloc.h>
#include <sys/device.h>
#include <sys/audioio.h>
#include <dev/audio_if.h>
#include <dev/mulaw.h>
#include "audio.h"
#include "wskbd.h"

#ifdef AUDIO_DEBUG
#define DPRINTF(...)				\
	do {					\
		if (audio_debug)		\
			printf(__VA_ARGS__);	\
	} while(0)
#define DPRINTFN(n, ...)			\
	do {					\
		if (audio_debug > (n))		\
			printf(__VA_ARGS__);	\
	} while(0)
#else
#define DPRINTF(...) do {} while(0)
#define DPRINTFN(n, ...) do {} while(0)
#endif

#define DEVNAME(sc)		((sc)->dev.dv_xname)
#define AUDIO_UNIT(n)		(minor(n) & 0x0f)
#define AUDIO_DEV(n)		(minor(n) & 0xf0)
#define AUDIO_DEV_AUDIO		0	/* minor of /dev/audio0 */
#define AUDIO_DEV_MIXER		0x10	/* minor of /dev/mixer0 */
#define AUDIO_DEV_AUDIOCTL	0xc0	/* minor of /dev/audioctl */
#define AUDIO_BUFSZ		65536	/* buffer size in bytes */

/*
 * dma buffer
 */
struct audio_buf {
	unsigned char *data;		/* DMA memory block */
	size_t datalen;			/* size of DMA memory block */
	size_t len;			/* size of DMA FIFO */
	size_t start;			/* first byte used in the FIFO */
	size_t used;			/* bytes used in the FIFO */
	size_t blksz;			/* DMA block size */
	struct selinfo sel;		/* to record & wakeup poll(2) */
	unsigned int pos;		/* bytes transferred */
	unsigned int xrun;		/* bytes lost by xruns */
	int blocking;			/* read/write blocking */
};

#if NWSKBD > 0
struct wskbd_vol
{
	int val;			/* index of the value control */
	int mute;			/* index of the mute control */
	int step;			/* increment/decrement step */
	int nch;			/* channels in the value control */
	int val_pending;		/* pending change of val */
	int mute_pending;		/* pending change of mute */
#define WSKBD_MUTE_TOGGLE	1
#define WSKBD_MUTE_DISABLE	2
#define WSKBD_MUTE_ENABLE	3
};
#endif

/*
 * device structure
 */
struct audio_softc {
	struct device dev;
	struct audio_hw_if *ops;	/* driver funcs */
	void *arg;			/* first arg to driver funcs */
	int mode;			/* bitmask of AUMODE_* */
	int quiesce;			/* device suspended */
	struct audio_buf play, rec;
	unsigned int sw_enc;		/* user exposed AUDIO_ENCODING_* */
	unsigned int hw_enc;		/* hardware AUDIO_ENCODING_* */
	unsigned int bits;		/* bits per sample */
	unsigned int bps;		/* bytes-per-sample */
	unsigned int msb;		/* sample are MSB aligned */
	unsigned int rate;		/* rate in Hz */
	unsigned int round;		/* block size in frames */
	unsigned int nblks;		/* number of play blocks */
	unsigned int pchan, rchan;	/* number of channels */
	unsigned char silence[4];	/* a sample of silence */
	int pause;			/* not trying to start DMA */
	int active;			/* DMA in process */
	int offs;			/* offset between play & rec dir */
	void (*conv_enc)(unsigned char *, int);	/* encode to native */
	void (*conv_dec)(unsigned char *, int);	/* decode to user */
#if NWSKBD > 0
	struct wskbd_vol spkr, mic;
	struct task wskbd_task;
	int wskbd_taskset;
#endif
};

int audio_match(struct device *, void *, void *);
void audio_attach(struct device *, struct device *, void *);
int audio_activate(struct device *, int);
int audio_detach(struct device *, int);
void audio_pintr(void *);
void audio_rintr(void *);
#if NWSKBD > 0
void wskbd_mixer_init(struct audio_softc *);
#endif

const struct cfattach audio_ca = {
	sizeof(struct audio_softc), audio_match, audio_attach,
	audio_detach, audio_activate
};

struct cfdriver audio_cd = {
	NULL, "audio", DV_DULL
};

/*
 * This mutex protects data structures (including registers on the
 * sound-card) that are manipulated by both the interrupt handler and
 * syscall code-paths.
 *
 * Note that driver methods may sleep (e.g. in malloc); consequently the
 * audio layer calls them with the mutex unlocked. Driver methods are
 * responsible for locking the mutex when they manipulate data used by
 * the interrupt handler and interrupts may occur.
 *
 * Similarly, the driver is responsible for locking the mutex in its
 * interrupt handler and to call the audio layer call-backs (i.e.
 * audio_{p,r}int()) with the mutex locked.
 */
struct mutex audio_lock = MUTEX_INITIALIZER(IPL_AUDIO);

#ifdef AUDIO_DEBUG
/*
 * 0 - nothing, as if AUDIO_DEBUG isn't defined
 * 1 - initialisations & setup
 * 2 - blocks & interrupts
 */
int audio_debug = 1;
#endif

unsigned int
audio_gcd(unsigned int a, unsigned int b)
{
	unsigned int r;

	while (b > 0) {
		r = a % b;
		a = b;
		b = r;
	}
	return a;
}

int
audio_buf_init(struct audio_softc *sc, struct audio_buf *buf, int dir)
{
	if (sc->ops->round_buffersize) {
		buf->datalen = sc->ops->round_buffersize(sc->arg,
		    dir, AUDIO_BUFSZ);
	} else
		buf->datalen = AUDIO_BUFSZ;
	if (sc->ops->allocm) {
		buf->data = sc->ops->allocm(sc->arg, dir, buf->datalen,
		    M_DEVBUF, M_WAITOK);
	} else
		buf->data = malloc(buf->datalen, M_DEVBUF, M_WAITOK);
	if (buf->data == NULL)
		return ENOMEM;
	return 0;
}

void
audio_buf_done(struct audio_softc *sc, struct audio_buf *buf)
{
	if (sc->ops->freem)
		sc->ops->freem(sc->arg, buf->data, M_DEVBUF);
	else
		free(buf->data, M_DEVBUF, buf->datalen);
}

/*
 * return the reader pointer and the number of bytes available
 */
unsigned char *
audio_buf_rgetblk(struct audio_buf *buf, size_t *rsize)
{
	size_t count;

	count = buf->len - buf->start;
	if (count > buf->used)
		count = buf->used;
	*rsize = count;
	return buf->data + buf->start;
}

/*
 * discard "count" bytes at the start position.
 */
void
audio_buf_rdiscard(struct audio_buf *buf, size_t count)
{
#ifdef AUDIO_DEBUG
	if (count > buf->used) {
		panic("audio_buf_rdiscard: bad count = %zu, "
		    "start = %zu, used = %zu\n", count, buf->start, buf->used);
	}
#endif
	buf->used -= count;
	buf->start += count;
	if (buf->start >= buf->len)
		buf->start -= buf->len;
}

/*
 * advance the writer pointer by "count" bytes
 */
void
audio_buf_wcommit(struct audio_buf *buf, size_t count)
{
#ifdef AUDIO_DEBUG
	if (count > (buf->len - buf->used)) {
		panic("audio_buf_wcommit: bad count = %zu, "
		    "start = %zu, used = %zu\n", count, buf->start, buf->used);
	}
#endif
	buf->used += count;
}

/*
 * get writer pointer and the number of bytes writable
 */
unsigned char *
audio_buf_wgetblk(struct audio_buf *buf, size_t *rsize)
{
	size_t end, avail, count;

	end = buf->start + buf->used;
	if (end >= buf->len)
		end -= buf->len;
	avail = buf->len - buf->used;
	count = buf->len - end;
	if (count > avail)
		count = avail;
	*rsize = count;
	return buf->data + end;
}

void
audio_calc_sil(struct audio_softc *sc)
{
	unsigned char *q;
	unsigned int s, i;
	int d, e;

	e = sc->sw_enc;
#ifdef AUDIO_DEBUG
	switch (e) {
	case AUDIO_ENCODING_SLINEAR_LE:
	case AUDIO_ENCODING_ULINEAR_LE:
	case AUDIO_ENCODING_SLINEAR_BE:
	case AUDIO_ENCODING_ULINEAR_BE:
		break;
	default:
		printf("%s: unhandled play encoding %d\n", DEVNAME(sc), e);
		memset(sc->silence, 0, sc->bps);
		return;
	}
#endif
	if (e == AUDIO_ENCODING_SLINEAR_BE || e == AUDIO_ENCODING_ULINEAR_BE) {
		d = -1;
		q = sc->silence + sc->bps - 1;
	} else {
		d = 1;
		q = sc->silence;
	}
	if (e == AUDIO_ENCODING_SLINEAR_LE || e == AUDIO_ENCODING_SLINEAR_BE) {
		s = 0;
	} else {
		s = 0x80000000;
		if (sc->msb)
			s >>= 32 - 8 * sc->bps;
		else
			s >>= 32 - sc->bits;
	}
	for (i = 0; i < sc->bps; i++) {
		*q = s;
		q += d;
		s >>= 8;
	}
	if (sc->conv_enc)
		sc->conv_enc(sc->silence, sc->bps);
}

void
audio_fill_sil(struct audio_softc *sc, unsigned char *ptr, size_t count)
{
	unsigned char *q, *p;
	size_t i, j;

	q = ptr;
	for (j = count / sc->bps; j > 0; j--) {
		p = sc->silence;
		for (i = sc->bps; i > 0; i--)
			*q++ = *p++;
	}
}

void
audio_clear(struct audio_softc *sc)
{
	if (sc->mode & AUMODE_PLAY) {
		sc->play.used = sc->play.start = 0;
		sc->play.pos = sc->play.xrun = 0;
		audio_fill_sil(sc, sc->play.data, sc->play.len);
	}
	if (sc->mode & AUMODE_RECORD) {
		sc->rec.used = sc->rec.start = 0;
		sc->rec.pos = sc->rec.xrun = 0;
		audio_fill_sil(sc, sc->rec.data, sc->rec.len);
	}
}

/*
 * called whenever a block is consumed by the driver
 */
void
audio_pintr(void *addr)
{
	struct audio_softc *sc = addr;
	unsigned char *ptr;
	size_t count;
	int error, nblk, todo;

	MUTEX_ASSERT_LOCKED(&audio_lock);
	if (!(sc->mode & AUMODE_PLAY) || !sc->active) {
		printf("%s: play interrupt but not playing\n", DEVNAME(sc));
		return;
	}
	if (sc->quiesce) {
		DPRINTF("%s: quiesced, skipping play intr\n", DEVNAME(sc));
		return;
	}

	/*
	 * check if record pointer wrapped, see explanation
	 * in audio_rintr()
	 */
	if (sc->mode & AUMODE_RECORD) {
		sc->offs--;
		nblk = sc->rec.len / sc->rec.blksz;
		todo = -sc->offs;
		if (todo >= nblk) {
			todo -= todo % nblk;
			DPRINTFN(1, "%s: rec ptr wrapped, moving %d blocks\n",
			    DEVNAME(sc), todo);
			while (todo-- > 0)
				audio_rintr(sc);
		}
	}

	sc->play.pos += sc->play.blksz;
	audio_fill_sil(sc, sc->play.data + sc->play.start, sc->play.blksz);
	audio_buf_rdiscard(&sc->play, sc->play.blksz);
	if (sc->play.used < sc->play.blksz) {
		DPRINTFN(1, "%s: play underrun\n", DEVNAME(sc));
		sc->play.xrun += sc->play.blksz;
		audio_buf_wcommit(&sc->play, sc->play.blksz);
	}

	DPRINTFN(1, "%s: play intr, used -> %zu, start -> %zu\n",
	    DEVNAME(sc), sc->play.used, sc->play.start);

	if (!sc->ops->trigger_output) {
		ptr = audio_buf_rgetblk(&sc->play, &count);
		error = sc->ops->start_output(sc->arg,
		    ptr, sc->play.blksz, audio_pintr, (void *)sc);
		if (error) {
			printf("%s: play restart failed: %d\n",
			    DEVNAME(sc), error);
		}
	}

	if (sc->play.used < sc->play.len) {
		DPRINTFN(1, "%s: play wakeup, chan = %d\n",
		    DEVNAME(sc), sc->play.blocking);
		if (sc->play.blocking) {
			wakeup(&sc->play.blocking);
			sc->play.blocking = 0;
		}
		selwakeup(&sc->play.sel);
	}
}

/*
 * called whenever a block is produced by the driver
 */
void
audio_rintr(void *addr)
{
	struct audio_softc *sc = addr;
	unsigned char *ptr;
	size_t count;
	int error, nblk, todo;

	MUTEX_ASSERT_LOCKED(&audio_lock);
	if (!(sc->mode & AUMODE_RECORD) || !sc->active) {
		printf("%s: rec interrupt but not recording\n", DEVNAME(sc));
		return;
	}
	if (sc->quiesce) {
		DPRINTF("%s: quiesced, skipping rec intr\n", DEVNAME(sc));
		return;
	}

	/*
	 * Interrupts may be masked by other sub-systems during 320ms
	 * and more. During such a delay the hardware doesn't stop
	 * playing and the play buffer pointers may wrap, this can't be
	 * detected and corrected by low level drivers. This makes the
	 * record stream ahead of the play stream; this is detected as a
	 * hardware anomaly by userland and cause programs to misbehave.
	 *
	 * We fix this by advancing play position by an integer count of
	 * full buffers, so it reaches the record position.
	 */
	if (sc->mode & AUMODE_PLAY) {
		sc->offs++;
		nblk = sc->play.len / sc->play.blksz;
		todo = sc->offs;
		if (todo >= nblk) {
			todo -= todo % nblk;
			DPRINTFN(1, "%s: play ptr wrapped, moving %d blocks\n",
			    DEVNAME(sc), todo);
			while (todo-- > 0)
				audio_pintr(sc);
		}
	}

	sc->rec.pos += sc->rec.blksz;
	audio_buf_wcommit(&sc->rec, sc->rec.blksz);
	if (sc->rec.used == sc->rec.len) {
		DPRINTFN(1, "%s: rec overrun\n", DEVNAME(sc));
		sc->rec.xrun += sc->rec.blksz;
		audio_buf_rdiscard(&sc->rec, sc->rec.blksz);
	}
	DPRINTFN(1, "%s: rec intr, used -> %zu\n", DEVNAME(sc), sc->rec.used);

	if (!sc->ops->trigger_input) {
		ptr = audio_buf_wgetblk(&sc->rec, &count);
		error = sc->ops->start_input(sc->arg,
		    ptr, sc->rec.blksz, audio_rintr, (void *)sc);
		if (error) {
			printf("%s: rec restart failed: %d\n",
			    DEVNAME(sc), error);
		}
	}

	if (sc->rec.used > 0) {
		DPRINTFN(1, "%s: rec wakeup, chan = %d\n",
		    DEVNAME(sc), sc->rec.blocking);
		if (sc->rec.blocking) {
			wakeup(&sc->rec.blocking);
			sc->rec.blocking = 0;
		}
		selwakeup(&sc->rec.sel);
	}
}

int
audio_start_do(struct audio_softc *sc)
{
	int error;
	struct audio_params p;
	unsigned char *ptr;
	size_t count;

	DPRINTF("%s: starting\n", DEVNAME(sc));

	error = 0;
	sc->offs = 0;
	if (sc->mode & AUMODE_PLAY) {
		if (sc->ops->trigger_output) {
			p.encoding = sc->hw_enc;
			p.precision = sc->bits;
			p.bps = sc->bps;
			p.msb = sc->msb;
			p.sample_rate = sc->rate;
			p.channels = sc->pchan;
			error = sc->ops->trigger_output(sc->arg,
			    sc->play.data,
			    sc->play.data + sc->play.len,
			    sc->play.blksz,
			    audio_pintr, (void *)sc, &p);
		} else {
			mtx_enter(&audio_lock);
			ptr = audio_buf_rgetblk(&sc->play, &count);
			error = sc->ops->start_output(sc->arg,
			    ptr, sc->play.blksz, audio_pintr, (void *)sc);
			mtx_leave(&audio_lock);
		}
		if (error)
			printf("%s: failed to start playback\n", DEVNAME(sc));
	}
	if (sc->mode & AUMODE_RECORD) {
		if (sc->ops->trigger_input) {
			p.encoding = sc->hw_enc;
			p.precision = sc->bits;
			p.bps = sc->bps;
			p.msb = sc->msb;
			p.sample_rate = sc->rate;
			p.channels = sc->rchan;
			error = sc->ops->trigger_input(sc->arg,
			    sc->rec.data,
			    sc->rec.data + sc->rec.len,
			    sc->rec.blksz,
			    audio_rintr, (void *)sc, &p);
		} else {
			mtx_enter(&audio_lock);
			ptr = audio_buf_wgetblk(&sc->rec, &count);
			error = sc->ops->start_input(sc->arg,
			    ptr, sc->rec.blksz, audio_rintr, (void *)sc);
			mtx_leave(&audio_lock);
		}
		if (error)
			printf("%s: failed to start recording\n", DEVNAME(sc));
	}
	return error;
}

int
audio_stop_do(struct audio_softc *sc)
{
	if (sc->mode & AUMODE_PLAY)
		sc->ops->halt_output(sc->arg);
	if (sc->mode & AUMODE_RECORD)
		sc->ops->halt_input(sc->arg);
	DPRINTF("%s: stopped\n", DEVNAME(sc));
	return 0;
}

int
audio_start(struct audio_softc *sc)
{
	sc->active = 1;
	sc->play.xrun = sc->play.pos = sc->rec.xrun = sc->rec.pos = 0;
	return audio_start_do(sc);
}

int
audio_stop(struct audio_softc *sc)
{
	int error;

	error = audio_stop_do(sc);
	if (error)
		return error;
	audio_clear(sc);
	sc->active = 0;
	return 0;
}

int
audio_canstart(struct audio_softc *sc)
{
	if (sc->active || sc->pause)
		return 0;
	if ((sc->mode & AUMODE_RECORD) && sc->rec.used != 0)
		return 0;
	if ((sc->mode & AUMODE_PLAY) && sc->play.used != sc->play.len)
		return 0;
	return 1;
}

int
audio_setpar(struct audio_softc *sc)
{
	struct audio_params p, r;
	unsigned int nr, np, max, min, mult;
	unsigned int blk_mult, blk_max;
	int error;

	DPRINTF("%s: setpar: req enc=%d bits=%d, bps=%d, msb=%d "
	    "rate=%d, pchan=%d, rchan=%d, round=%u, nblks=%d\n",
	    DEVNAME(sc), sc->sw_enc, sc->bits, sc->bps, sc->msb,
	    sc->rate, sc->pchan, sc->rchan, sc->round, sc->nblks);

	/*
	 * check if requested parameters are in the allowed ranges
	 */
	if (sc->mode & AUMODE_PLAY) {
		if (sc->pchan < 1)
			sc->pchan = 1;
		else if (sc->pchan > 64)
			sc->pchan = 64;
	}
	if (sc->mode & AUMODE_RECORD) {
		if (sc->rchan < 1)
			sc->rchan = 1;
		else if (sc->rchan > 64)
			sc->rchan = 64;
	}
	switch (sc->sw_enc) {
	case AUDIO_ENCODING_ULAW:
	case AUDIO_ENCODING_ALAW:
	case AUDIO_ENCODING_SLINEAR_LE:
	case AUDIO_ENCODING_SLINEAR_BE:
	case AUDIO_ENCODING_ULINEAR_LE:
	case AUDIO_ENCODING_ULINEAR_BE:
		break;
	default:
		sc->sw_enc = AUDIO_ENCODING_SLINEAR_LE;
	}
	if (sc->bits < 8)
		sc->bits = 8;
	else if (sc->bits > 32)
		sc->bits = 32;
	if (sc->bps < 1)
		sc->bps = 1;
	else if (sc->bps > 4)
		sc->bps = 4;
	if (sc->rate < 4000)
		sc->rate = 4000;
	else if (sc->rate > 192000)
		sc->rate = 192000;

	/*
	 * copy into struct audio_params, required by drivers
	 */
	p.encoding = r.encoding = sc->sw_enc;
	p.precision = r.precision = sc->bits;
	p.bps = r.bps = sc->bps;
	p.msb = r.msb = sc->msb;
	p.sample_rate = r.sample_rate = sc->rate;
	p.channels = sc->pchan;
	r.channels = sc->rchan;

	/*
	 * set parameters
	 */
	error = sc->ops->set_params(sc->arg, sc->mode, sc->mode, &p, &r);
	if (error)
		return error;
	if (sc->mode == (AUMODE_PLAY | AUMODE_RECORD)) {
		if (p.encoding != r.encoding ||
		    p.precision != r.precision ||
		    p.bps != r.bps ||
		    p.msb != r.msb ||
		    p.sample_rate != r.sample_rate) {
			printf("%s: different play and record parameters "
			    "returned by hardware\n", DEVNAME(sc));
			return ENODEV;
		}
	}
	if (sc->mode & AUMODE_PLAY) {
		sc->hw_enc = p.encoding;
		sc->bits = p.precision;
		sc->bps = p.bps;
		sc->msb = p.msb;
		sc->rate = p.sample_rate;
		sc->pchan = p.channels;
	}
	if (sc->mode & AUMODE_RECORD) {
		sc->hw_enc = r.encoding;
		sc->bits = r.precision;
		sc->bps = r.bps;
		sc->msb = r.msb;
		sc->rate = r.sample_rate;
		sc->rchan = r.channels;
	}
	if (sc->rate == 0 || sc->bps == 0 || sc->bits == 0) {
		printf("%s: invalid parameters returned by hardware\n",
		    DEVNAME(sc));
		return ENODEV;
	}
	if (sc->ops->commit_settings) {
		error = sc->ops->commit_settings(sc->arg);
		if (error)
			return error;
	}

	/*
	 * conversion from/to exotic/dead encoding, for drivers not supporting
	 * linear
	 */
	switch (sc->hw_enc) {
	case AUDIO_ENCODING_SLINEAR_LE:
	case AUDIO_ENCODING_SLINEAR_BE:
	case AUDIO_ENCODING_ULINEAR_LE:
	case AUDIO_ENCODING_ULINEAR_BE:
		sc->sw_enc = sc->hw_enc;
		sc->conv_dec = sc->conv_enc = NULL;
		break;
	case AUDIO_ENCODING_ULAW:
#if BYTE_ORDER == LITTLE_ENDIAN
		sc->sw_enc = AUDIO_ENCODING_SLINEAR_LE;
#else
		sc->sw_enc = AUDIO_ENCODING_SLINEAR_BE;
#endif
		if (sc->bits == 8) {
			sc->conv_enc = slinear8_to_mulaw;
			sc->conv_dec = mulaw_to_slinear8;
		} else if (sc->bits == 24) {
			sc->conv_enc = slinear24_to_mulaw24;
			sc->conv_dec = mulaw24_to_slinear24;
		} else {
			sc->sw_enc = sc->hw_enc;
			sc->conv_dec = sc->conv_enc = NULL;
		}
		break;
	default:
		printf("%s: setpar: enc = %d, bits = %d: emulation skipped\n",
		    DEVNAME(sc), sc->hw_enc, sc->bits);
		sc->sw_enc = sc->hw_enc;
		sc->conv_dec = sc->conv_enc = NULL;
	}
	audio_calc_sil(sc);

	/*
	 * get least multiplier of the number of frames per block
	 */
	if (sc->ops->round_blocksize) {
		blk_mult = sc->ops->round_blocksize(sc->arg, 1);
		if (blk_mult == 0) {
			printf("%s: 0x%x: bad block size multiplier\n",
			    DEVNAME(sc), blk_mult);
			return ENODEV;
		}
	} else
		blk_mult = 1;
	DPRINTF("%s: hw block size multiplier: %u\n", DEVNAME(sc), blk_mult);
	if (sc->mode & AUMODE_PLAY) {
		np = blk_mult / audio_gcd(sc->pchan * sc->bps, blk_mult);
		if (!(sc->mode & AUMODE_RECORD))
			nr = np;
		DPRINTF("%s: play number of frames multiplier: %u\n",
		    DEVNAME(sc), np);
	}
	if (sc->mode & AUMODE_RECORD) {
		nr = blk_mult / audio_gcd(sc->rchan * sc->bps, blk_mult);
		if (!(sc->mode & AUMODE_PLAY))
			np = nr;
		DPRINTF("%s: record number of frames multiplier: %u\n",
		    DEVNAME(sc), nr);
	}
	mult = nr * np / audio_gcd(nr, np);
	DPRINTF("%s: least common number of frames multiplier: %u\n",
	    DEVNAME(sc), mult);

	/*
	 * get minimum and maximum frames per block
	 */
	if (sc->ops->round_blocksize)
		blk_max = sc->ops->round_blocksize(sc->arg, AUDIO_BUFSZ);
	else
		blk_max = AUDIO_BUFSZ;
	if ((sc->mode & AUMODE_PLAY) && blk_max > sc->play.datalen / 2)
		blk_max = sc->play.datalen / 2;
	if ((sc->mode & AUMODE_RECORD) && blk_max > sc->rec.datalen / 2)
		blk_max = sc->rec.datalen / 2;
	if (sc->mode & AUMODE_PLAY) {
		np = blk_max / (sc->pchan * sc->bps);
		if (!(sc->mode & AUMODE_RECORD))
			nr = np;
	}
	if (sc->mode & AUMODE_RECORD) {
		nr = blk_max / (sc->rchan * sc->bps);
		if (!(sc->mode & AUMODE_PLAY))
			np = nr;
	}
	max = np < nr ? np : nr;
	max -= max % mult;
	min = sc->rate / 1000 + mult - 1;
	min -= min % mult;
	DPRINTF("%s: frame number range: %u..%u\n", DEVNAME(sc), min, max);
	if (max < min) {
		printf("%s: %u: bad max frame number\n", DEVNAME(sc), max);
		return EIO;
	}

	/*
	 * adjust the frame per block to match our constraints
	 */
	sc->round += mult / 2;
	sc->round -= sc->round % mult;
	if (sc->round > max)
		sc->round = max;
	else if (sc->round < min)
		sc->round = min;
	sc->round = sc->round;

	/*
	 * set buffer size (number of blocks)
	 */
	if (sc->mode & AUMODE_PLAY) {
		sc->play.blksz = sc->round * sc->pchan * sc->bps;
		max = sc->play.datalen / sc->play.blksz;
		if (sc->nblks > max)
			sc->nblks = max;
		else if (sc->nblks < 2)
			sc->nblks = 2;
		sc->play.len = sc->nblks * sc->play.blksz;
		sc->nblks = sc->nblks;
	}
	if (sc->mode & AUMODE_RECORD) {
		/*
		 * for recording, buffer size is not the latency (it's
		 * exactly one block), so let's get the maximum buffer
		 * size of maximum reliability during xruns
		 */
		sc->rec.blksz = sc->round * sc->rchan * sc->bps;
		sc->rec.len = sc->rec.datalen;
		sc->rec.len -= sc->rec.datalen % sc->rec.blksz;
	}

	DPRINTF("%s: setpar: new enc=%d bits=%d, bps=%d, msb=%d "
	    "rate=%d, pchan=%d, rchan=%d, round=%u, nblks=%d\n",
	    DEVNAME(sc), sc->sw_enc, sc->bits, sc->bps, sc->msb,
	    sc->rate, sc->pchan, sc->rchan, sc->round, sc->nblks);
	return 0;
}

int
audio_ioc_start(struct audio_softc *sc)
{
	if (!sc->pause) {
		DPRINTF("%s: can't start: already started\n", DEVNAME(sc));
		return EBUSY;
	}
	if ((sc->mode & AUMODE_PLAY) && sc->play.used != sc->play.len) {
		DPRINTF("%s: play buffer not ready\n", DEVNAME(sc));
		return EBUSY;
	}
	if ((sc->mode & AUMODE_RECORD) && sc->rec.used != 0) {
		DPRINTF("%s: record buffer not ready\n", DEVNAME(sc));
		return EBUSY;
	}
	sc->pause = 0;
	return audio_start(sc);
}

int
audio_ioc_stop(struct audio_softc *sc)
{
	if (sc->pause) {
		DPRINTF("%s: can't stop: not started\n", DEVNAME(sc));
		return EBUSY;
	}
	sc->pause = 1;
	if (sc->active)
		return audio_stop(sc);
	return 0;
}

int
audio_ioc_getpar(struct audio_softc *sc, struct audio_swpar *p)
{
	p->rate = sc->rate;
	p->sig = sc->sw_enc == AUDIO_ENCODING_SLINEAR_LE ||
	    sc->sw_enc == AUDIO_ENCODING_SLINEAR_BE;
	p->le = sc->sw_enc == AUDIO_ENCODING_SLINEAR_LE ||
	    sc->sw_enc == AUDIO_ENCODING_ULINEAR_LE;
	p->bits = sc->bits;
	p->bps = sc->bps;
	p->msb = sc->msb;
	p->pchan = sc->pchan;
	p->rchan = sc->rchan;
	p->nblks = sc->nblks;
	p->round = sc->round;
	return 0;
}

int
audio_ioc_setpar(struct audio_softc *sc, struct audio_swpar *p)
{
	int error, le, sig;

	if (sc->active) {
		DPRINTF("%s: can't change params during dma\n",
		    DEVNAME(sc));
		return EBUSY;
	}

	/*
	 * copy desired parameters into the softc structure
	 */
	if (p->sig != ~0U || p->le != ~0U || p->bits != ~0U) {
		sig = 1;
		le = (BYTE_ORDER == LITTLE_ENDIAN);
		sc->bits = 16;
		sc->bps = 2;
		sc->msb = 1;
		if (p->sig != ~0U)
			sig = p->sig;
		if (p->le != ~0U)
			le = p->le;
		if (p->bits != ~0U) {
			sc->bits = p->bits;
			sc->bps = sc->bits <= 8 ?
			    1 : (sc->bits <= 16 ? 2 : 4);
			if (p->bps != ~0U)
				sc->bps = p->bps;
			if (p->msb != ~0U)
				sc->msb = p->msb ? 1 : 0;
		}
		sc->sw_enc = (sig) ?
		    (le ? AUDIO_ENCODING_SLINEAR_LE :
			AUDIO_ENCODING_SLINEAR_BE) :
		    (le ? AUDIO_ENCODING_ULINEAR_LE :
			AUDIO_ENCODING_ULINEAR_BE);
	}
	if (p->rate != ~0)
		sc->rate = p->rate;
	if (p->pchan != ~0)
		sc->pchan = p->pchan;
	if (p->rchan != ~0)
		sc->rchan = p->rchan;
	if (p->round != ~0)
		sc->round = p->round;
	if (p->nblks != ~0)
		sc->nblks = p->nblks;

	/*
	 * if the device is not opened for playback or recording don't
	 * touch the hardware yet (ex. if this is /dev/audioctlN)
	 */
	if (sc->mode == 0)
		return 0;

	/*
	 * negociate parameters with the hardware
	 */
	error = audio_setpar(sc);
	if (error)
		return error;
	audio_clear(sc);
	if ((sc->mode & AUMODE_PLAY) && sc->ops->init_output) {
		error = sc->ops->init_output(sc->arg,
		    sc->play.data, sc->play.len);
		if (error)
			return error;
	}
	if ((sc->mode & AUMODE_RECORD) && sc->ops->init_input) {
		error = sc->ops->init_input(sc->arg,
		    sc->rec.data, sc->rec.len);
		if (error)
			return error;
	}
	return 0;
}

int
audio_ioc_getstatus(struct audio_softc *sc, struct audio_status *p)
{
	p->mode = sc->mode;
	p->pause = sc->pause;
	p->active = sc->active;
	return 0;
}

int
audio_match(struct device *parent, void *match, void *aux)
{
	struct audio_attach_args *sa = aux;

	return (sa->type == AUDIODEV_TYPE_AUDIO) ? 1 : 0;
}

void
audio_attach(struct device *parent, struct device *self, void *aux)
{
	struct audio_softc *sc = (void *)self;
	struct audio_attach_args *sa = aux;
	struct audio_hw_if *ops = sa->hwif;
	void *arg = sa->hdl;
	int error;

	printf("\n");

#ifdef DIAGNOSTIC
	if (ops == 0 ||
	    ops->open == 0 ||
	    ops->close == 0 ||
	    ops->set_params == 0 ||
	    (ops->start_output == 0 && ops->trigger_output == 0) ||
	    (ops->start_input == 0 && ops->trigger_input == 0) ||
	    ops->halt_output == 0 ||
	    ops->halt_input == 0 ||
	    ops->set_port == 0 ||
	    ops->get_port == 0 ||
	    ops->query_devinfo == 0 ||
	    ops->get_props == 0) {
		printf("%s: missing method\n", DEVNAME(sc));
		sc->ops = 0;
		return;
	}
#endif
	sc->ops = ops;
	sc->arg = arg;

#if NWSKBD > 0
	wskbd_mixer_init(sc);
#endif /* NWSKBD > 0 */

	error = audio_buf_init(sc, &sc->play, AUMODE_PLAY);
	if (error) {
		sc->ops = 0;
		printf("%s: could not allocate play buffer\n", DEVNAME(sc));
		return;
	}
	error = audio_buf_init(sc, &sc->rec, AUMODE_RECORD);
	if (error) {
		audio_buf_done(sc, &sc->play);
		sc->ops = 0;
		printf("%s: could not allocate record buffer\n", DEVNAME(sc));
		return;
	}

	/* set defaults */
#if BYTE_ORDER == LITTLE_ENDIAN
	sc->sw_enc = AUDIO_ENCODING_SLINEAR_LE;
#else
	sc->sw_enc = AUDIO_ENCODING_SLINEAR_BE;
#endif
	sc->bits = 16;
	sc->bps = 2;
	sc->msb = 1;
	sc->rate = 48000;
	sc->pchan = 2;
	sc->rchan = 2;
	sc->round = 960;
	sc->nblks = 2;
	sc->play.pos = sc->play.xrun = sc->rec.pos = sc->rec.xrun = 0;
}

int
audio_activate(struct device *self, int act)
{
	struct audio_softc *sc = (struct audio_softc *)self;

	switch (act) {
	case DVACT_QUIESCE:
		/*
		 * good drivers run play and rec handlers in a single
		 * interrupt. Grab the lock to ensure we expose the same
		 * sc->quiesce value to both play and rec handlers
		 */
		mtx_enter(&audio_lock);
		sc->quiesce = 1;
		mtx_leave(&audio_lock);

		/*
		 * once sc->quiesce is set, interrupts may occur, but
		 * counters are not advanced and consequently processes
		 * keep sleeping.
		 *
		 * XXX: ensure read/write/ioctl don't start/stop
		 * DMA at the same time, this needs a "ready" condvar
		 */
		if (sc->mode != 0 && sc->active)
			audio_stop_do(sc);
		DPRINTF("%s: quiesce: active = %d\n", DEVNAME(sc), sc->active);
		break;
	case DVACT_WAKEUP:
		DPRINTF("%s: wakeup: active = %d\n", DEVNAME(sc), sc->active);

		/*
		 * keep buffer usage the same, but set start pointer to
		 * the beginning of the buffer.
		 *
		 * No need to grab the audio_lock as DMA is stopped and
		 * this is the only thread running (caller ensures this)
		 */
		sc->quiesce = 0;
		wakeup(&sc->quiesce);

		if (sc->mode != 0) {
			if (audio_setpar(sc) != 0)
				break;
			if (sc->mode & AUMODE_PLAY) {
				sc->play.start = 0;
				audio_fill_sil(sc, sc->play.data, sc->play.len);
			}
			if (sc->mode & AUMODE_RECORD) {
				sc->rec.start = sc->rec.len - sc->rec.used;
				audio_fill_sil(sc, sc->rec.data, sc->rec.len);
			}
			if (sc->active)
				audio_start_do(sc);
		}
		break;
	}
	return 0;
}

int
audio_detach(struct device *self, int flags)
{
	struct audio_softc *sc = (struct audio_softc *)self;
	int maj, mn;

	DPRINTF("%s: audio_detach: flags = %d\n", DEVNAME(sc), flags);

	wakeup(&sc->quiesce);

	/* locate the major number */
	for (maj = 0; maj < nchrdev; maj++)
		if (cdevsw[maj].d_open == audioopen)
			break;
	/*
	 * Nuke the vnodes for any open instances, calls close but as
	 * close uses device_lookup, it returns EXIO and does nothing
	 */
	mn = self->dv_unit;
	vdevgone(maj, mn | AUDIO_DEV_AUDIO, mn | AUDIO_DEV_AUDIO, VCHR);
	vdevgone(maj, mn | AUDIO_DEV_AUDIOCTL, mn | AUDIO_DEV_AUDIOCTL, VCHR);
	vdevgone(maj, mn | AUDIO_DEV_MIXER, mn | AUDIO_DEV_MIXER, VCHR);

	/*
	 * The close() method did nothing, quickly halt DMA (normally
	 * parent is already gone, and code below is no-op), and wake-up
	 * user-land blocked in read/write/ioctl, which return EIO.
	 */
	if (sc->mode != 0) {
		if (sc->active) {
			wakeup(&sc->play.blocking);
			selwakeup(&sc->play.sel);
			wakeup(&sc->rec.blocking);
			selwakeup(&sc->rec.sel);
			audio_stop(sc);
		}
		sc->ops->close(sc->arg);
		sc->mode = 0;
	}

	/* free resources */
	audio_buf_done(sc, &sc->play);
	audio_buf_done(sc, &sc->rec);
	return 0;
}

int
audio_submatch(struct device *parent, void *match, void *aux)
{
        struct cfdata *cf = match;

	return (cf->cf_driver == &audio_cd);
}

struct device *
audio_attach_mi(struct audio_hw_if *ops, void *arg, struct device *dev)
{
	struct audio_attach_args aa;

	aa.type = AUDIODEV_TYPE_AUDIO;
	aa.hwif = ops;
	aa.hdl = arg;

	/*
	 * attach this driver to the caller (hardware driver), this
	 * checks the kernel config and possibly calls audio_attach()
	 */
	return config_found_sm(dev, &aa, audioprint, audio_submatch);
}

int
audioprint(void *aux, const char *pnp)
{
	struct audio_attach_args *arg = aux;
	const char *type;

	if (pnp != NULL) {
		switch (arg->type) {
		case AUDIODEV_TYPE_AUDIO:
			type = "audio";
			break;
		case AUDIODEV_TYPE_OPL:
			type = "opl";
			break;
		case AUDIODEV_TYPE_MPU:
			type = "mpu";
			break;
		default:
			panic("audioprint: unknown type %d", arg->type);
		}
		printf("%s at %s", type, pnp);
	}
	return UNCONF;
}

int
audio_open(struct audio_softc *sc, int flags)
{
	int error;
	int props;

	if (sc->mode)
		return EBUSY;
	error = sc->ops->open(sc->arg, flags);
	if (error)
		return error;
	sc->active = 0;
	sc->pause = 1;
	sc->rec.blocking = 0;
	sc->play.blocking = 0;
	sc->mode = 0;
	if (flags & FWRITE)
		sc->mode |= AUMODE_PLAY;
	if (flags & FREAD)
		sc->mode |= AUMODE_RECORD;
	props = sc->ops->get_props(sc->arg);
	if (sc->mode == (AUMODE_PLAY | AUMODE_RECORD)) {
		if (!(props & AUDIO_PROP_FULLDUPLEX)) {
			error = ENOTTY;
			goto bad;
		}
		if (sc->ops->setfd) {
			error = sc->ops->setfd(sc->arg, 1);
			if (error)
				goto bad;
		}
	}

	if (sc->ops->speaker_ctl) {
		/*
		 * XXX: what is this used for?
		 */
		sc->ops->speaker_ctl(sc->arg,
		    (sc->mode & AUMODE_PLAY) ? SPKR_ON : SPKR_OFF);
	}

	error = audio_setpar(sc);
	if (error)
		goto bad;
	audio_clear(sc);

	/*
	 * allow read(2)/write(2) to automatically start DMA, without
	 * the need for ioctl(), to make /dev/audio usable in scripts
	 */
	sc->pause = 0;
	return 0;
bad:
	sc->ops->close(sc->arg);
	sc->mode = 0;
	return error;
}

int
audio_drain(struct audio_softc *sc)
{
	int error, xrun;
	unsigned char *ptr;
	size_t count, bpf;

	DPRINTF("%s: drain: mode = %d, pause = %d, active = %d, used = %zu\n",
	    DEVNAME(sc), sc->mode, sc->pause, sc->active, sc->play.used);
	if (!(sc->mode & AUMODE_PLAY) || sc->pause)
		return 0;

	/* discard partial samples, required by audio_fill_sil() */
	mtx_enter(&audio_lock);
	bpf = sc->pchan * sc->bps;
	sc->play.used -= sc->play.used % bpf;
	if (sc->play.used == 0) {
		mtx_leave(&audio_lock);
		return 0;
	}

	if (!sc->active) {
		/*
		 * dma not started yet because buffer was not full
		 * enough to start automatically. Pad it and start now.
		 */
		for (;;) {
			ptr = audio_buf_wgetblk(&sc->play, &count);
			if (count == 0)
				break;
			audio_fill_sil(sc, ptr, count);
			audio_buf_wcommit(&sc->play, count);
		}
		mtx_leave(&audio_lock);
		error = audio_start(sc);
		if (error)
			return error;
		mtx_enter(&audio_lock);
	}

	xrun = sc->play.xrun;
	while (sc->play.xrun == xrun) {
		DPRINTF("%s: drain: used = %zu, xrun = %d\n",
		    DEVNAME(sc), sc->play.used, sc->play.xrun);

		/*
		 * set a 5 second timeout, in case interrupts don't
		 * work, useful only for debugging drivers
		 */
		sc->play.blocking = 1;
		error = msleep(&sc->play.blocking, &audio_lock,
		    PWAIT | PCATCH, "au_dr", 5 * hz);
		if (!(sc->dev.dv_flags & DVF_ACTIVE))
			error = EIO;
		if (error) {
			DPRINTF("%s: drain, err = %d\n", DEVNAME(sc), error);
			break;
		}
	}
	mtx_leave(&audio_lock);
	return error;
}

int
audio_close(struct audio_softc *sc)
{
	audio_drain(sc);
	if (sc->active)
		audio_stop(sc);
	sc->ops->close(sc->arg);
	sc->mode = 0;
	DPRINTF("%s: close: done\n", DEVNAME(sc));
	return 0;
}

int
audio_read(struct audio_softc *sc, struct uio *uio, int ioflag)
{
	unsigned char *ptr;
	size_t count;
	int error;

	DPRINTFN(1, "%s: read: resid = %zd\n", DEVNAME(sc), uio->uio_resid);

	/* block if quiesced */
	while (sc->quiesce)
		tsleep(&sc->quiesce, 0, "au_qrd", 0);

	/* start automatically if audio_ioc_start() was never called */
	mtx_enter(&audio_lock);
	if (audio_canstart(sc)) {
		mtx_leave(&audio_lock);
		error = audio_start(sc);
		if (error)
			return error;
		mtx_enter(&audio_lock);
	}

	/* if there is no data then sleep */
	while (sc->rec.used == 0) {
		if (ioflag & IO_NDELAY) {
			mtx_leave(&audio_lock);
			return EWOULDBLOCK;
		}
		DPRINTFN(1, "%s: read sleep\n", DEVNAME(sc));
		sc->rec.blocking = 1;
		error = msleep(&sc->rec.blocking,
		    &audio_lock, PWAIT | PCATCH, "au_rd", 0);
		if (!(sc->dev.dv_flags & DVF_ACTIVE))
			error = EIO;
		if (error) {
			DPRINTF("%s: read woke up error = %d\n",
			    DEVNAME(sc), error);
			mtx_leave(&audio_lock);
			return error;
		}
	}

	/* at this stage, there is data to transfer */
	while (uio->uio_resid > 0 && sc->rec.used > 0) {
		ptr = audio_buf_rgetblk(&sc->rec, &count);
		if (count > uio->uio_resid)
			count = uio->uio_resid;
		audio_buf_rdiscard(&sc->rec, count);
		mtx_leave(&audio_lock);
		DPRINTFN(1, "%s: read: start = %zu, count = %zu\n",
		    DEVNAME(sc), ptr - sc->rec.data, count);
		if (sc->conv_dec)
			sc->conv_dec(ptr, count);
		error = uiomove(ptr, count, uio);
		if (error)
			return error;
		mtx_enter(&audio_lock);
	}
	mtx_leave(&audio_lock);
	return 0;
}

int
audio_write(struct audio_softc *sc, struct uio *uio, int ioflag)
{
	unsigned char *ptr;
	size_t count;
	int error;

	DPRINTFN(1, "%s: write: resid = %zd\n",  DEVNAME(sc), uio->uio_resid);

	/* block if quiesced */
	while (sc->quiesce)
		tsleep(&sc->quiesce, 0, "au_qwr", 0);

	/*
	 * if IO_NDELAY flag is set then check if there is enough room
	 * in the buffer to store at least one byte. If not then don't
	 * start the write process.
	 */
	mtx_enter(&audio_lock);
	if (uio->uio_resid > 0 && (ioflag & IO_NDELAY)) {
		if (sc->play.used == sc->play.len) {
			mtx_leave(&audio_lock);
			return EWOULDBLOCK;
		}
	}

	while (uio->uio_resid > 0) {
		while (1) {
			ptr = audio_buf_wgetblk(&sc->play, &count);
			if (count > 0)
				break;
			if (ioflag & IO_NDELAY) {
				/*
				 * At this stage at least one byte is already
				 * moved so we do not return EWOULDBLOCK
				 */
				mtx_leave(&audio_lock);
				return 0;
			}
			DPRINTFN(1, "%s: write sleep\n", DEVNAME(sc));
			sc->play.blocking = 1;
			error = msleep(&sc->play.blocking,
			    &audio_lock, PWAIT | PCATCH, "au_wr", 0);
			if (!(sc->dev.dv_flags & DVF_ACTIVE))
				error = EIO;
			if (error) {
				DPRINTF("%s: write woke up error = %d\n",
				    DEVNAME(sc), error);
				mtx_leave(&audio_lock);
				return error;
			}
		}
		if (count > uio->uio_resid)
			count = uio->uio_resid;
		audio_buf_wcommit(&sc->play, count);
		mtx_leave(&audio_lock);
		error = uiomove(ptr, count, uio);
		if (error)
			return 0;
		if (sc->conv_enc) {
			sc->conv_enc(ptr, count);
			DPRINTFN(1, "audio_write: converted count = %zu\n",
			    count);
		}

		/* start automatically if audio_ioc_start() was never called */
		if (audio_canstart(sc)) {
			error = audio_start(sc);
			if (error)
				return error;
		}
		mtx_enter(&audio_lock);
	}
	mtx_leave(&audio_lock);
	return 0;
}

int
audio_getdev(struct audio_softc *sc, struct audio_device *adev)
{
	memset(adev, 0, sizeof(struct audio_device));
	if (sc->dev.dv_parent == NULL)
		return EIO;
	strlcpy(adev->name, sc->dev.dv_parent->dv_xname, MAX_AUDIO_DEV_LEN);
	return 0;
}

int
audio_ioctl(struct audio_softc *sc, unsigned long cmd, void *addr)
{
	struct audio_pos *ap;
	int error = 0;

	/* block if quiesced */
	while (sc->quiesce)
		tsleep(&sc->quiesce, 0, "au_qio", 0);

	switch (cmd) {
	case FIONBIO:
		/* All handled in the upper FS layer. */
		break;
	case AUDIO_GETPOS:
		mtx_enter(&audio_lock);
		ap = (struct audio_pos *)addr;
		ap->play_pos = sc->play.pos;
		ap->play_xrun = sc->play.xrun;
		ap->rec_pos = sc->rec.pos;
		ap->rec_xrun = sc->rec.xrun;
		mtx_leave(&audio_lock);
		break;
	case AUDIO_START:
		return audio_ioc_start(sc);
	case AUDIO_STOP:
		return audio_ioc_stop(sc);
	case AUDIO_SETPAR:
		error = audio_ioc_setpar(sc, (struct audio_swpar *)addr);
		break;
	case AUDIO_GETPAR:
		error = audio_ioc_getpar(sc, (struct audio_swpar *)addr);
		break;
	case AUDIO_GETSTATUS:
		error = audio_ioc_getstatus(sc, (struct audio_status *)addr);
		break;
	case AUDIO_GETDEV:
		error = audio_getdev(sc, (struct audio_device *)addr);
		break;
	default:
		DPRINTF("%s: unknown ioctl 0x%lx\n", DEVNAME(sc), cmd);
		error = ENOTTY;
		break;
	}
	return error;
}

int
audio_ioctl_mixer(struct audio_softc *sc, unsigned long cmd, void *addr)
{
	int error;

	/* block if quiesced */
	while (sc->quiesce)
		tsleep(&sc->quiesce, 0, "mix_qio", 0);

	switch (cmd) {
	case FIONBIO:
		/* All handled in the upper FS layer. */
		break;
	case AUDIO_MIXER_DEVINFO:
		((mixer_devinfo_t *)addr)->un.v.delta = 0;
		return sc->ops->query_devinfo(sc->arg, (mixer_devinfo_t *)addr);
	case AUDIO_MIXER_READ:
		return sc->ops->get_port(sc->arg, (mixer_ctrl_t *)addr);
	case AUDIO_MIXER_WRITE:
		error = sc->ops->set_port(sc->arg, (mixer_ctrl_t *)addr);
		if (error)
			return error;
		if (sc->ops->commit_settings)
			return sc->ops->commit_settings(sc->arg);
		break;
	default:
		return ENOTTY;
	}
	return 0;
}

int
audio_poll(struct audio_softc *sc, int events, struct proc *p)
{
	int revents = 0;

	mtx_enter(&audio_lock);
	if ((sc->mode & AUMODE_RECORD) && sc->rec.used > 0)
		revents |= events & (POLLIN | POLLRDNORM);
	if ((sc->mode & AUMODE_PLAY) && sc->play.used < sc->play.len)
		revents |= events & (POLLOUT | POLLWRNORM);
	if (revents == 0) {
		if (events & (POLLIN | POLLRDNORM))
			selrecord(p, &sc->rec.sel);
		if (events & (POLLOUT | POLLWRNORM))
			selrecord(p, &sc->play.sel);
	}
	mtx_leave(&audio_lock);
	return revents;
}

int
audioopen(dev_t dev, int flags, int mode, struct proc *p)
{
	struct audio_softc *sc;
	int error;

	sc = (struct audio_softc *)device_lookup(&audio_cd, AUDIO_UNIT(dev));
	if (sc == NULL)
		return ENXIO;
	if (sc->ops == NULL)
		error = ENXIO;
	else {
		switch (AUDIO_DEV(dev)) {
		case AUDIO_DEV_AUDIO:
			error = audio_open(sc, flags);
			break;
		case AUDIO_DEV_AUDIOCTL:
		case AUDIO_DEV_MIXER:
			error = 0;
			break;
		default:
			error = ENXIO;
		}
	}
	device_unref(&sc->dev);
	return error;
}

int
audioclose(dev_t dev, int flags, int ifmt, struct proc *p)
{
	struct audio_softc *sc;
	int error;

	sc = (struct audio_softc *)device_lookup(&audio_cd, AUDIO_UNIT(dev));
	if (sc == NULL)
		return ENXIO;
	switch (AUDIO_DEV(dev)) {
	case AUDIO_DEV_AUDIO:
		error = audio_close(sc);
		break;
	case AUDIO_DEV_MIXER:
	case AUDIO_DEV_AUDIOCTL:
		error = 0;
		break;
	default:
		error = ENXIO;
	}
	device_unref(&sc->dev);
	return error;
}

int
audioread(dev_t dev, struct uio *uio, int ioflag)
{
	struct audio_softc *sc;
	int error;

	sc = (struct audio_softc *)device_lookup(&audio_cd, AUDIO_UNIT(dev));
	if (sc == NULL)
		return ENXIO;
	switch (AUDIO_DEV(dev)) {
	case AUDIO_DEV_AUDIO:
		error = audio_read(sc, uio, ioflag);
		break;
	case AUDIO_DEV_AUDIOCTL:
	case AUDIO_DEV_MIXER:
		error = ENODEV;
		break;
	default:
		error = ENXIO;
	}
	device_unref(&sc->dev);
	return error;
}

int
audiowrite(dev_t dev, struct uio *uio, int ioflag)
{
	struct audio_softc *sc;
	int error;

	sc = (struct audio_softc *)device_lookup(&audio_cd, AUDIO_UNIT(dev));
	if (sc == NULL)
		return ENXIO;
	switch (AUDIO_DEV(dev)) {
	case AUDIO_DEV_AUDIO:
		error = audio_write(sc, uio, ioflag);
		break;
	case AUDIO_DEV_AUDIOCTL:
	case AUDIO_DEV_MIXER:
		error = ENODEV;
		break;
	default:
		error = ENXIO;
	}
	device_unref(&sc->dev);
	return error;
}

int
audioioctl(dev_t dev, u_long cmd, caddr_t addr, int flag, struct proc *p)
{
	struct audio_softc *sc;
	int error;

	sc = (struct audio_softc *)device_lookup(&audio_cd, AUDIO_UNIT(dev));
	if (sc == NULL)
		return ENXIO;
	switch (AUDIO_DEV(dev)) {
	case AUDIO_DEV_AUDIO:
		error = audio_ioctl(sc, cmd, addr);
		break;
	case AUDIO_DEV_AUDIOCTL:
		if (cmd == AUDIO_SETPAR && sc->mode != 0) {
			error = EBUSY;
			break;
		}
		if (cmd == AUDIO_START || cmd == AUDIO_STOP) {
			error = ENXIO;
			break;
		}
		error = audio_ioctl(sc, cmd, addr);
		break;
	case AUDIO_DEV_MIXER:
		error = audio_ioctl_mixer(sc, cmd, addr);
		break;
	default:
		error = ENXIO;
	}
	device_unref(&sc->dev);
	return error;
}

int
audiopoll(dev_t dev, int events, struct proc *p)
{
	struct audio_softc *sc;
	int revents;

	sc = (struct audio_softc *)device_lookup(&audio_cd, AUDIO_UNIT(dev));
	if (sc == NULL)
		return POLLERR;
	switch (AUDIO_DEV(dev)) {
	case AUDIO_DEV_AUDIO:
		revents = audio_poll(sc, events, p);
		break;
	case AUDIO_DEV_AUDIOCTL:
	case AUDIO_DEV_MIXER:
	default:
		revents = 0;
		break;
	}
	device_unref(&sc->dev);
	return revents;
}

#if NWSKBD > 0
int
wskbd_initmute(struct audio_softc *sc, struct mixer_devinfo *vol)
{
	struct mixer_devinfo *mi;
	int index = -1;

	mi = malloc(sizeof(struct mixer_devinfo), M_TEMP, M_WAITOK);

	for (mi->index = vol->next; mi->index != -1; mi->index = mi->next) {
		if (sc->ops->query_devinfo(sc->arg, mi) != 0)
			break;
		if (strcmp(mi->label.name, AudioNmute) == 0) {
			index = mi->index;
			break;
		}
	}

	free(mi, M_TEMP, sizeof(struct mixer_devinfo));
	return index;
}

int
wskbd_initvol(struct audio_softc *sc, struct wskbd_vol *vol, char *cn, char *dn)
{
	struct mixer_devinfo *dev, *cls;

	vol->val = vol->mute = -1;
	dev = malloc(sizeof(struct mixer_devinfo), M_TEMP, M_WAITOK);
	cls = malloc(sizeof(struct mixer_devinfo), M_TEMP, M_WAITOK);

	for (dev->index = 0; ; dev->index++) {
		if (sc->ops->query_devinfo(sc->arg, dev) != 0)
			break;
		if (dev->type != AUDIO_MIXER_VALUE)
			continue;
		cls->index = dev->mixer_class;
		if (sc->ops->query_devinfo(sc->arg, cls) != 0)
			continue;
		if (strcmp(cls->label.name, cn) == 0 &&
		    strcmp(dev->label.name, dn) == 0) {
			vol->val = dev->index;
			vol->nch = dev->un.v.num_channels;
			vol->step = dev->un.v.delta > 8 ? dev->un.v.delta : 8;
			vol->mute = wskbd_initmute(sc, dev);
			vol->val_pending = vol->mute_pending = 0;
			DPRINTF("%s: wskbd using %s.%s%s\n", DEVNAME(sc),
			    cn, dn, vol->mute >= 0 ? ", mute control" : "");
			break;
		}
	}

	free(cls, M_TEMP, sizeof(struct mixer_devinfo));
	free(dev, M_TEMP, sizeof(struct mixer_devinfo));
	return (vol->val != -1);
}

void
wskbd_mixer_init(struct audio_softc *sc)
{
	static struct {
		char *cn, *dn;
	} spkr_names[] = {
		{AudioCoutputs, AudioNmaster},
		{AudioCinputs,  AudioNdac},
		{AudioCoutputs, AudioNdac},
		{AudioCoutputs, AudioNoutput}
	}, mic_names[] = {
		{AudioCrecord, AudioNrecord},
		{AudioCrecord, AudioNvolume},
		{AudioCinputs, AudioNrecord},
		{AudioCinputs, AudioNvolume},
		{AudioCinputs, AudioNinput}
	};
	int i;

	if (sc->dev.dv_unit != 0) {
		DPRINTF("%s: not configuring wskbd keys\n", DEVNAME(sc));
		return;
	}
	for (i = 0; i < sizeof(spkr_names) / sizeof(spkr_names[0]); i++) {
		if (wskbd_initvol(sc, &sc->spkr,
			spkr_names[i].cn, spkr_names[i].dn))
			break;
	}
	for (i = 0; i < sizeof(mic_names) / sizeof(mic_names[0]); i++) {
		if (wskbd_initvol(sc, &sc->mic,
			mic_names[i].cn, mic_names[i].dn))
			break;
	}
}

void
wskbd_mixer_update(struct audio_softc *sc, struct wskbd_vol *vol)
{
	struct mixer_ctrl ctrl;
	int val_pending, mute_pending, i, gain, error, s;

	s = spltty();
	val_pending = vol->val_pending;
	vol->val_pending = 0;
	mute_pending = vol->mute_pending;
	vol->mute_pending = 0;
	splx(s);

	if (sc->ops == NULL)
		return;
	if (vol->mute >= 0 && mute_pending) {
		ctrl.dev = vol->mute;
		ctrl.type = AUDIO_MIXER_ENUM;
		error = sc->ops->get_port(sc->arg, &ctrl);
		if (error) {
			DPRINTF("%s: get mute err = %d\n", DEVNAME(sc), error);
			return;
		}
		switch (mute_pending) {
		case WSKBD_MUTE_TOGGLE:
			ctrl.un.ord = !ctrl.un.ord;
			break;
		case WSKBD_MUTE_DISABLE:
			ctrl.un.ord = 0;
			break;
		case WSKBD_MUTE_ENABLE:
			ctrl.un.ord = 1;
			break;
		}
		DPRINTFN(1, "%s: wskbd mute setting to %d\n",
		    DEVNAME(sc), ctrl.un.ord);
		error = sc->ops->set_port(sc->arg, &ctrl);
		if (error) {
			DPRINTF("%s: set mute err = %d\n", DEVNAME(sc), error);
			return;
		}
	}
	if (vol->val >= 0 && val_pending) {
		ctrl.dev = vol->val;
		ctrl.type = AUDIO_MIXER_VALUE;
		ctrl.un.value.num_channels = vol->nch;
		error = sc->ops->get_port(sc->arg, &ctrl);
		if (error) {
			DPRINTF("%s: get mute err = %d\n", DEVNAME(sc), error);
			return;
		}
		for (i = 0; i < vol->nch; i++) {
			gain = ctrl.un.value.level[i] + vol->step * val_pending;
			if (gain > AUDIO_MAX_GAIN)
				gain = AUDIO_MAX_GAIN;
			else if (gain < AUDIO_MIN_GAIN)
				gain = AUDIO_MIN_GAIN;
			ctrl.un.value.level[i] = gain;
			DPRINTFN(1, "%s: wskbd level %d set to %d\n",
			    DEVNAME(sc), i, gain);
		}
		error = sc->ops->set_port(sc->arg, &ctrl);
		if (error) {
			DPRINTF("%s: set vol err = %d\n", DEVNAME(sc), error);
			return;
		}
	}
}

void
wskbd_mixer_cb(void *addr)
{
	struct audio_softc *sc = addr;
	int s;

	wskbd_mixer_update(sc, &sc->spkr);
	wskbd_mixer_update(sc, &sc->mic);
	s = spltty();
	sc->wskbd_taskset = 0;
	splx(s);
	device_unref(&sc->dev);
}

int
wskbd_set_mixermute(long mute, long out)
{
	struct audio_softc *sc;
	struct wskbd_vol *vol;

	sc = (struct audio_softc *)device_lookup(&audio_cd, 0);
	if (sc == NULL)
		return ENODEV;
	vol = out ? &sc->spkr : &sc->mic;
	vol->mute_pending = mute ? WSKBD_MUTE_ENABLE : WSKBD_MUTE_DISABLE;
	if (!sc->wskbd_taskset) {
		task_set(&sc->wskbd_task, wskbd_mixer_cb, sc);
		task_add(systq, &sc->wskbd_task);
		sc->wskbd_taskset = 1;
	}
	return 0;
}

int
wskbd_set_mixervolume(long dir, long out)
{
	struct audio_softc *sc;
	struct wskbd_vol *vol;

	sc = (struct audio_softc *)device_lookup(&audio_cd, 0);
	if (sc == NULL)
		return ENODEV;
	vol = out ? &sc->spkr : &sc->mic;
	if (dir == 0)
		vol->mute_pending ^= WSKBD_MUTE_TOGGLE;
	else
		vol->val_pending += dir;
	if (!sc->wskbd_taskset) {
		task_set(&sc->wskbd_task, wskbd_mixer_cb, sc);
		task_add(systq, &sc->wskbd_task);
		sc->wskbd_taskset = 1;
	}
	return 0;
}
#endif /* NWSKBD > 0 */
