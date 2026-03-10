/*
 * Copyright (c) 2026 joshua stein <jcs@jcs.org>
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
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/malloc.h>
#include <sys/tty.h>

#include <machine/bus.h>
#include <machine/fdt.h>

#include <dev/ic/comreg.h>
#include <dev/ic/comvar.h>

#include <dev/ofw/fdt.h>
#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_gpio.h>
#include <dev/ofw/ofw_pinctrl.h>

struct bcmbt_softc {
	struct device sc_dev;
	int sc_node;

	struct com_softc *sc_com;
};

int	bcmbt_match(struct device *, void *, void *);
void	bcmbt_attach(struct device *, struct device *, void *);
void	bcmbt_load_firmware(struct device *);

int	bcmbt_uart_init(struct bcmbt_softc *, int);
int	bcmbt_uart_putc(struct bcmbt_softc *, uint8_t);
int	bcmbt_uart_getc(struct bcmbt_softc *, uint8_t *, int);
int	bcmbt_send_cmd(struct bcmbt_softc *, const uint8_t *, int);
int	bcmbt_recv_evt(struct bcmbt_softc *, uint8_t *, int, int *);

static const uint8_t hci_reset[] = { 0x01, 0x03, 0x0c, 0x00 };
static const uint8_t hci_download_minidrv[] = { 0x01, 0x2e, 0xfc, 0x00 };
static const uint8_t hci_read_bd_addr[] = { 0x01, 0x09, 0x10, 0x00 };

const struct cfattach bcmbt_ca = {
	sizeof(struct bcmbt_softc), bcmbt_match, bcmbt_attach
};

struct cfdriver bcmbt_cd = {
	NULL, "bcmbt", DV_DULL
};

int
bcmbt_match(struct device *parent, void *match, void *aux)
{
	struct fdt_attach_args *faa = aux;

	return OF_is_compatible(faa->fa_node, "brcm,bcm43438-bt");
}

void
bcmbt_attach(struct device *parent, struct device *self, void *aux)
{
	struct bcmbt_softc *sc = (struct bcmbt_softc *)self;
	struct fdt_attach_args *faa = aux;

	sc->sc_com = (struct com_softc *)parent;
	sc->sc_node = faa->fa_node;

	config_mountroot(self, bcmbt_load_firmware);

	printf("\n");
}

void
bcmbt_load_firmware(struct device *self)
{
	struct bcmbt_softc *sc = (struct bcmbt_softc *)self;
	uint8_t resp[260];
	char fwname[20];
	uint32_t *gpio;
	int gpios_len;
	uint8_t *fw, *p, junk;
	size_t fwlen;
	int rlen, cmdlen, error;

	/* deassert then assert to power-cycle the chip */
	gpios_len = OF_getproplen(sc->sc_node, "shutdown-gpios");
	if (gpios_len > 0) {
		gpio = malloc(gpios_len, M_TEMP, M_WAITOK);
		OF_getpropintarray(sc->sc_node, "shutdown-gpios", gpio,
		    gpios_len);

		/* power off, wait 200ms, power on */
		gpio_controller_config_pin(gpio, GPIO_CONFIG_OUTPUT);
		gpio_controller_set_pin(gpio, 0);
		delay(200000);
		gpio_controller_set_pin(gpio, 1);
		free(gpio, M_TEMP, gpios_len);
	}

	/* RTS pulse to wake the BCM43430 UART */
	gpios_len = OF_getproplen(sc->sc_node, "uart-rts-gpios");
	if (gpios_len > 0) {
		gpio = malloc(gpios_len, M_TEMP, M_WAITOK);
		OF_getpropintarray(sc->sc_node, "uart-rts-gpios", gpio,
		    gpios_len);

		/* switch to GPIO mode */
		pinctrl_byname(sc->sc_node, "rts-gpio");
		gpio_controller_config_pin(gpio, GPIO_CONFIG_OUTPUT);
		gpio_controller_set_pin(gpio, 1);
		delay(100000);
		gpio_controller_set_pin(gpio, 0);

		/* then back to UART mode */
		pinctrl_byname(sc->sc_node, "rts-uart");
		free(gpio, M_TEMP, gpios_len);
	}

	gpios_len = OF_getproplen(sc->sc_node, "device-wakeup-gpios");
	if (gpios_len > 0) {
		gpio = malloc(gpios_len, M_TEMP, M_WAITOK);
		OF_getpropintarray(sc->sc_node, "device-wakeup-gpios", gpio,
		    gpios_len);
		gpio_controller_config_pin(gpio, GPIO_CONFIG_OUTPUT);
		gpio_controller_set_pin(gpio, 1);
		free(gpio, M_TEMP, gpios_len);
	}

	/* let the chip boot after reset */
	delay(500000);

	if (bcmbt_uart_init(sc, 115200) != 0) {
		printf("%s: uart init failed\n", sc->sc_dev.dv_xname);
		return;
	}

	/* drain any stale data */
	while (bcmbt_uart_getc(sc, &junk, 10000) == 0)
		continue;

	/* send initial HCI Reset */
	if (bcmbt_send_cmd(sc, hci_reset, sizeof(hci_reset)) != 0 ||
	    bcmbt_recv_evt(sc, resp, sizeof(resp), &rlen) != 0) {
		printf("%s: HCI reset failed\n", sc->sc_dev.dv_xname);
		return;
	}
	delay(100000);

	/* enter download mode */
	if (bcmbt_send_cmd(sc, hci_download_minidrv,
	    sizeof(hci_download_minidrv)) != 0 ||
	    bcmbt_recv_evt(sc, resp, sizeof(resp), &rlen) != 0) {
		printf("%s: download minidrv failed\n",
		    sc->sc_dev.dv_xname);
		return;
	}
	delay(50000);

	/* TODO: per-chipset firmware files */
	snprintf(fwname, sizeof(fwname), "BCM4343A1.hcd");
	error = loadfirmware(fwname, &fw, &fwlen);
	if (error) {
		printf("%s: failed to load firmware %s (error %d)\n",
		    sc->sc_dev.dv_xname, fwname, error);
		return;
	}

	/* send hcd firmware commands, prepending 0x01 HCI command byte */
	p = fw;
	while (p < fw + fwlen) {
		if (p + 3 > fw + fwlen)
			break;

		cmdlen = 3 + p[2];
		if (p + cmdlen > fw + fwlen) {
			printf("%s: firmware %s truncated\n",
			    sc->sc_dev.dv_xname, fwname);
			free(fw, M_DEVBUF, fwlen);
			return;
		}

		if (bcmbt_uart_putc(sc, 0x01) != 0 ||
		    bcmbt_send_cmd(sc, p, cmdlen) != 0 ||
		    bcmbt_recv_evt(sc, resp, sizeof(resp), &rlen) != 0) {
			printf("%s: firmware command failed\n",
			    sc->sc_dev.dv_xname);
			free(fw, M_DEVBUF, fwlen);
			return;
		}

		p += cmdlen;
	}
	free(fw, M_DEVBUF, fwlen);

	/* wait for chip reset after firmware download */
	delay(250000);

	/* re-initialize UART after chip reset */
	if (bcmbt_uart_init(sc, 115200) != 0) {
		printf("%s: post-reset init failed\n", sc->sc_dev.dv_xname);
		return;
	}

	/* drain any boot diagnostic data */
	while (bcmbt_uart_getc(sc, &junk, 10000) == 0)
		continue;

	/* send post-firmware HCI Reset */
	if (bcmbt_send_cmd(sc, hci_reset, sizeof(hci_reset)) != 0 ||
	    bcmbt_recv_evt(sc, resp, sizeof(resp), &rlen) != 0) {
		printf("%s: post-firmware HCI reset failed\n",
		    sc->sc_dev.dv_xname);
		return;
	}
	delay(100000);

	/* read BD_ADDR */
	if (bcmbt_send_cmd(sc, hci_read_bd_addr,
	    sizeof(hci_read_bd_addr)) != 0 ||
	    bcmbt_recv_evt(sc, resp, sizeof(resp), &rlen) != 0 ||
	    rlen < 13 || resp[6] != 0x00) {
		printf("%s: failed to read BD_ADDR\n",
		    sc->sc_dev.dv_xname);
		return;
	}

	printf("%s: address %02x:%02x:%02x:%02x:%02x:%02x\n",
	    sc->sc_dev.dv_xname,
	    resp[12], resp[11], resp[10], resp[9], resp[8], resp[7]);
}

int
bcmbt_uart_init(struct bcmbt_softc *sc, int speed)
{
	int divisor, timo;

	com_write_reg(sc->sc_com, com_ier, 0);

	/* clear DW APB busy detect */
	com_read_reg(sc->sc_com, com_usr);

	divisor = sc->sc_com->sc_frequency / (16 * speed);

	for (timo = 100000; timo > 0; timo--) {
		com_write_reg(sc->sc_com, com_lctl, LCR_DLAB);
		if (com_read_reg(sc->sc_com, com_lctl) & LCR_DLAB)
			break;
		com_read_reg(sc->sc_com, com_usr);
		delay(1);
	}
	if (timo == 0)
		return -1;
	com_write_reg(sc->sc_com, com_dlbl, divisor & 0xff);
	com_write_reg(sc->sc_com, com_dlbh, (divisor >> 8) & 0xff);

	com_read_reg(sc->sc_com, com_usr);
	com_write_reg(sc->sc_com, com_lctl, LCR_8BITS);

	com_write_reg(sc->sc_com, com_fifo,
	    FIFO_ENABLE | FIFO_RCV_RST | FIFO_XMT_RST | FIFO_TRIGGER_1);

	com_write_reg(sc->sc_com, com_mcr, MCR_DTR | MCR_RTS);

	return 0;
}

int
bcmbt_uart_putc(struct bcmbt_softc *sc, uint8_t c)
{
	int timo;

	for (timo = 100000; timo > 0; timo--) {
		if (com_read_reg(sc->sc_com, com_lsr) & LSR_TXRDY) {
			com_write_reg(sc->sc_com, com_data, c);
			return 0;
		}
		delay(1);
	}
	return -1;
}

int
bcmbt_uart_getc(struct bcmbt_softc *sc, uint8_t *cp, int timo_us)
{
	int timo;

	for (timo = timo_us; timo > 0; timo--) {
		if (com_read_reg(sc->sc_com, com_lsr) & LSR_RXRDY) {
			*cp = com_read_reg(sc->sc_com, com_data);
			return 0;
		}
		delay(1);
	}
	return -1;
}

int
bcmbt_send_cmd(struct bcmbt_softc *sc, const uint8_t *cmd, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		if (bcmbt_uart_putc(sc, cmd[i]) != 0)
			return -1;
	}

	return 0;
}

int
bcmbt_recv_evt(struct bcmbt_softc *sc, uint8_t *buf, int buflen, int *rlen)
{
	uint8_t c;
	int plen;

	*rlen = 0;

	/* HCI event indicator: 0x04 */
	if (bcmbt_uart_getc(sc, &c, 1000000) != 0)
		return -1;
	if (c != 0x04)
		return -1;
	buf[(*rlen)++] = c;

	/* event code */
	if (bcmbt_uart_getc(sc, &c, 500000) != 0)
		return -1;
	buf[(*rlen)++] = c;

	/* parameter length */
	if (bcmbt_uart_getc(sc, &c, 500000) != 0)
		return -1;
	buf[(*rlen)++] = c;
	plen = c;

	/* parameter data */
	while (plen > 0) {
		if (*rlen >= buflen)
			return -1;
		if (bcmbt_uart_getc(sc, &c, 500000) != 0)
			return -1;
		buf[(*rlen)++] = c;
		plen--;
	}

	return 0;
}
