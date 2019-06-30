/*	$OpenBSD: usbhidaction.c,v 1.22 2016/03/17 19:40:43 krw Exp $ */
/*      $NetBSD: usbhidaction.c,v 1.7 2002/01/18 14:38:59 augustss Exp $ */

/*
 * Copyright (c) 2000, 2002 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Lennart Augustsson <lennart@augustsson.net>.
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
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/device.h>
#include <sys/ioctl.h>
#include <sys/hotplug.h>
#include <dev/usb/usb.h>
#include <dev/usb/usbhid.h>
#include <usbhid.h>
#include <syslog.h>
#include <signal.h>
#include <paths.h>
#include <poll.h>

int verbose = 0;
int isdemon = 0;

volatile sig_atomic_t reparse = 0;

struct command {
	struct command *next;
	int line;

	struct hid_item item;
	int value;
	char anyvalue;
	char *name;
	char *action;
};
struct command *commands;

/* max configuration line size */
#define SIZE 4000

#define HOTPLUG "/dev/hotplug"

void usage(void);
struct command *parse_conf(const char *, report_desc_t, int, int);
void docmd(struct command *, int, char *, int, char **);
void freecommands(struct command *);
int usbwait(long, long, int, char *);
int usbmatches(char *, long, long, int, char *);

/* ARGSUSED */
static void
sighup(int signo)
{
	reparse = 1;
}

int
main(int argc, char **argv)
{
	const char *conf = NULL;
	char dev[PATH_MAX] = { 0 };
	int fd, ch, sz, n, val, i;
	int demon, ignore;
	report_desc_t repd;
	char buf[100];
	char devnamebuf[PATH_MAX];
	const char *errstr;
	struct command *cmd;
	long usbv, usbp;
	int reportid = -1;

	demon = 1;
	ignore = 0;
	while ((ch = getopt(argc, argv, "c:df:ir:u:v")) != -1) {
		switch(ch) {
		case 'c':
			conf = optarg;
			break;
		case 'd':
			demon ^= 1;
			break;
		case 'i':
			ignore++;
			break;
		case 'f':
			strlcpy(dev, optarg, sizeof(dev));
			break;
		case 'r':
			reportid = strtonum(optarg, 0, INT_MAX, &errstr);
                        if (errstr != NULL)
                                errx(1, "reportid %s", errstr);
			break;
		case 'u':
			if (strlen(optarg) != 9 || optarg[4] != ':')
				errx(1, "-u vendor and product must be in "
				    "the form of xxxx:xxxx");
			memcpy(buf, optarg, 4);
			buf[4] = '\0';
			if ((usbv = strtol(buf, NULL, 16)) == 0)
				errx(1, "invalid USB vendor");
			memcpy(buf, optarg + 5, 4);
			buf[4] = '\0';
			if ((usbp = strtol(buf, NULL, 16)) == 0)
				errx(1, "invalid USB product");
			break;
		case 'v':
			demon = 0;
			verbose++;
			break;
		case '?':
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (conf == NULL)
		usage();
	if (!dev[0] && (!usbv || !usbp))
		usage();
	if (usbv && usbp && reportid < 0)
		usage();
	if (dev[0] && usbv && usbp) {
		warnx("-f and -u are mutually exclusive");
		usage();
	}

	if (hid_start(NULL) == -1)
		errx(1, "hid_init");

	if (demon && conf[0] != '/')
		errx(1, "config file must have an absolute path, %s", conf);

	if (dev[0]) {
		if (dev[0] != '/') {
			snprintf(devnamebuf, sizeof(devnamebuf), "/dev/%s%s",
			    isdigit((unsigned char)dev[0]) ? "uhid" : "", dev);
			strlcpy(dev, devnamebuf, sizeof(dev));
		}

		fd = open(dev, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			err(1, "%s", dev);
	}

	if (demon) {
		if (daemon(0, 0) < 0)
			err(1, "daemon()");
		isdemon = 1;
	}

	if (!dev[0]) {
		usbwait(usbv, usbp, reportid, dev);
		fd = open(dev, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			err(1, "%s", dev);
	}

	(void)signal(SIGHUP, sighup);

	/* we do not care about the children, so ignore them */
	(void)signal(SIGCHLD, SIG_IGN);

	if (reportid == -1) {
		if (ioctl(fd, USB_GET_REPORT_ID, &reportid) < 0)
			reportid = -1;
	}

	repd = hid_get_report_desc(fd);
	if (repd == NULL)
		err(1, "hid_get_report_desc() failed");

	commands = parse_conf(conf, repd, reportid, ignore);

	sz = hid_report_size(repd, hid_input, reportid);

	if (verbose)
		printf("report size %d\n", sz);
	if (sz > sizeof buf)
		errx(1, "report too large");

	for(;;) {
		n = read(fd, buf, sz);
		if (verbose > 2) {
			printf("read %d bytes:", n);
			for (i = 0; i < n; i++)
				printf(" %02x", buf[i]);
			printf("\n");
		}
		if (n < 0) {
			if (verbose)
				err(1, "read");
			else
				exit(1);
		}
		if (n != sz) {
			err(2, "read size");
		}
		for (cmd = commands; cmd; cmd = cmd->next) {
			val = hid_get_data(buf, &cmd->item);
			if (cmd->value == val || cmd->anyvalue)
				docmd(cmd, val, dev, argc, argv);
		}
		if (reparse) {
			struct command *cmds =
			    parse_conf(conf, repd, reportid, ignore);
			if (cmds) {
				freecommands(commands);
				commands = cmds;
			}
			reparse = 0;
		}
	}

	exit(0);
}

void
usage(void)
{
	extern char *__progname;

	fprintf(stderr, "usage: %s [-div] -c config-file -f device arg ...\n",
	    __progname);
	fprintf(stderr, "       %s [-div] -c config-file -r reportid "
	    "-u vend:prod arg ...\n", __progname);
	exit(1);
}

static int
peek(FILE *f)
{
	int c;

	c = getc(f);
	if (c != EOF)
		ungetc(c, f);
	return c;
}

struct command *
parse_conf(const char *conf, report_desc_t repd, int reportid, int ignore)
{
	FILE *f;
	char *p;
	int line;
	char buf[SIZE], name[SIZE], value[SIZE], action[SIZE];
	char usage[SIZE], coll[SIZE];
	struct command *cmd, *cmds;
	struct hid_data *d;
	struct hid_item h;
	int u, lo, hi, range;

	f = fopen(conf, "r");
	if (f == NULL)
		err(1, "%s", conf);

	cmds = NULL;
	for (line = 1; ; line++) {
		if (fgets(buf, sizeof buf, f) == NULL)
			break;
		if (buf[0] == '#' || buf[0] == '\n')
			continue;
		p = strchr(buf, '\n');
		while (p && isspace(peek(f))) {
			if (fgets(p, sizeof buf - strlen(buf), f) == NULL)
				break;
			p = strchr(buf, '\n');
		}
		if (p)
			*p = 0;
		if (sscanf(buf, "%s %s %[^\n]", name, value, action) != 3) {
			if (isdemon) {
				syslog(LOG_WARNING, "config file `%s', line %d"
				    ", syntax error: %s", conf, line, buf);
				freecommands(cmds);
				fclose(f);
				return (NULL);
			} else {
				errx(1, "config file `%s', line %d"
				    ", syntax error: %s", conf, line, buf);
			}
		}

		cmd = malloc(sizeof *cmd);
		if (cmd == NULL)
			err(1, "malloc failed");
		cmd->next = cmds;
		cmds = cmd;
		cmd->line = line;

		if (strcmp(value, "*") == 0) {
			cmd->anyvalue = 1;
		} else {
			cmd->anyvalue = 0;
			if (sscanf(value, "%d", &cmd->value) != 1) {
				if (isdemon) {
					syslog(LOG_WARNING,
					    "config file `%s', line %d, "
					    "bad value: %s",
					    conf, line, value);
					freecommands(cmds);
					fclose(f);
					return (NULL);
				} else {
					errx(1, "config file `%s', line %d, "
					    "bad value: %s",
					    conf, line, value);
				}
			}
		}

		coll[0] = 0;
		d = hid_start_parse(repd, 1 << hid_input, reportid);
		if (d == NULL)
			err(1, "hid_start_parse failed");
		while (hid_get_item(d, &h)) {
			if (verbose > 2)
				printf("kind=%d usage=%x\n", h.kind, h.usage);
			if (h.flags & HIO_CONST)
				continue;
			switch (h.kind) {
			case hid_input:
				if (h.usage_minimum != 0 ||
				    h.usage_maximum != 0) {
					lo = h.usage_minimum;
					hi = h.usage_maximum;
					range = 1;
				} else {
					lo = h.usage;
					hi = h.usage;
					range = 0;
				}
				for (u = lo; u <= hi; u++) {
					snprintf(usage, sizeof usage,  "%s:%s",
						 hid_usage_page(HID_PAGE(u)),
						 hid_usage_in_page(u));
					if (verbose > 2)
						printf("usage %s\n", usage);
					if (!strcasecmp(usage, name))
						goto foundhid;
					if (coll[0]) {
						snprintf(usage, sizeof usage,
						  "%s.%s:%s", coll+1,
						  hid_usage_page(HID_PAGE(u)),
						  hid_usage_in_page(u));
						if (verbose > 2)
							printf("usage %s\n",
							    usage);
						if (!strcasecmp(usage, name))
							goto foundhid;
					}
				}
				break;
			case hid_collection:
				snprintf(coll + strlen(coll),
				    sizeof coll - strlen(coll),  ".%s:%s",
				    hid_usage_page(HID_PAGE(h.usage)),
				    hid_usage_in_page(h.usage));
				break;
			case hid_endcollection:
				if (coll[0])
					*strrchr(coll, '.') = 0;
				break;
			default:
				break;
			}
		}
		hid_end_parse(d);
		if (ignore) {
			if (verbose)
				warnx("ignore item '%s'", name);
			/* pop and free this ignored item */
			cmds = cmd->next;
			free(cmd);
			continue;
		}
		if (isdemon) {
			syslog(LOG_WARNING, "config file `%s', line %d, HID "
			    "item not found: `%s'", conf, line, name);
			freecommands(cmds);
			fclose(f);
			return (NULL);
		} else {
			errx(1, "config file `%s', line %d, HID item "
			    "not found: `%s'", conf, line, name);
		}

	foundhid:
		hid_end_parse(d);
		cmd->item = h;
		cmd->name = strdup(name);
		cmd->action = strdup(action);
		if (range) {
			if (cmd->value == 1)
				cmd->value = u - lo;
			else
				cmd->value = -1;
		}

		if (verbose)
			printf("PARSE:%d %s, %d, '%s'\n", cmd->line, name,
			    cmd->value, cmd->action);
	}
	fclose(f);
	return (cmds);
}

void
docmd(struct command *cmd, int value, char *hid, int argc, char **argv)
{
	char cmdbuf[SIZE], *p, *q;
	size_t len;
	int n, r;
	pid_t pid;

	if (cmd->action == NULL) {
		if (verbose)
			printf("no action for device %s value %d\n",
			    hid, value);
		return;
	}
	for (p = cmd->action, q = cmdbuf; *p && q < &cmdbuf[SIZE-1]; ) {
		if (*p == '$') {
			p++;
			len = &cmdbuf[SIZE-1] - q;
			if (isdigit((unsigned char)*p)) {
				n = strtol(p, &p, 10) - 1;
				if (n >= 0 && n < argc) {
					strncpy(q, argv[n], len);
					q += strlen(q);
				}
			} else if (*p == 'V') {
				p++;
				snprintf(q, len, "%d", value);
				q += strlen(q);
			} else if (*p == 'N') {
				p++;
				strncpy(q, cmd->name, len);
				q += strlen(q);
			} else if (*p == 'H') {
				p++;
				strncpy(q, hid, len);
				q += strlen(q);
			} else if (*p) {
				*q++ = *p++;
			}
		} else {
			*q++ = *p++;
		}
	}
	*q = 0;

	pid = fork();
	if (pid == -1)
		warn("fork failed");
	else if (pid == 0) {
		setpgid(0, 0);
		if (verbose)
			printf("executing '%s'\n", cmdbuf);
		r = execl(_PATH_BSHELL, "sh", "-c", cmdbuf, (char *)NULL);
		err(1, "execl");
	}
}

void
freecommands(struct command *cmd)
{
	struct command *next;

	while (cmd) {
		next = cmd->next;
		free(cmd);
		cmd = next;
	}
}

int
usbwait(long usbv, long usbp, int reportid, char *dev)
{
	struct hotplug_event he;
	struct pollfd pfd[1];
	char *buf;
	int i, devfd, sc;

	if ((buf = malloc(PATH_MAX)) == NULL)
		err(1, "malloc");
	for (i = 0; i < 8; i++) {
		snprintf(buf, PATH_MAX, "/dev/uhid%d", i);
		if (usbmatches(buf, usbv, usbp, reportid, dev) == 0)
			return 0;
	}
	free(buf);

	if ((devfd = open(HOTPLUG, O_RDONLY | O_CLOEXEC)) == -1)
		err(1, "%s", HOTPLUG);

	/* flush events */
	pfd[0].fd = devfd;
	pfd[0].events = POLLIN;
	while (poll(pfd, 1, 0) > 0)
		read(devfd, &he, sizeof(he));

	for (;;) {
		if (read(devfd, &he, sizeof(he)) == -1) {
			printf("read -1\n");
			if (errno == EINTR)
				continue;
			err(1, "failed reading from " HOTPLUG);
		}

		if (he.he_type == HOTPLUG_DEVAT &&
		    sscanf(he.he_devname, "uhid%d", &sc) == 1) {
			snprintf(buf, PATH_MAX, "/dev/%s", he.he_devname);
			if (usbmatches(buf, usbv, usbp, reportid, dev) == 0) {
				close(devfd);
				return 0;
			}
		}
	}

	return 1;
}

int
usbmatches(char *dev, long usbv, long usbp, int reportid, char *out)
{
	int devfd, treportid;
	struct usb_device_info udi;

	if ((devfd = open(dev, O_RDONLY)) == -1) {
		if (verbose)
			warn("%s", dev);
		return 1;
	}
	if (ioctl(devfd, USB_GET_DEVICEINFO, &udi) < 0) {
		if (verbose)
			warn("%s: ioctl USB_GET_DEVICEINFO failed", dev);
		goto nomatch;
	}
	if (udi.udi_vendorNo != usbv || udi.udi_productNo != usbp) {
		if (verbose)
			printf("%s: vendor:%04x product %04x != wanted "
			    "%04lx:%04lx\n", dev, udi.udi_vendorNo,
			    udi.udi_productNo, usbv, usbp);
		goto nomatch;
	}
	if (ioctl(devfd, USB_GET_REPORT_ID, &treportid) < 0) {
		warn("%s: ioctl USB_GET_REPORTID failed", dev);
		goto nomatch;
	}
	if (treportid != reportid) {
		if (verbose)
			printf("%s: report id %d != wanted %d\n", dev,
			    treportid, reportid);
		goto nomatch;
	}

	close(devfd);

	if (verbose)
		printf("%s: found matching vendor %04lx product %04lx "
		    "report id %d\n", dev, usbv, usbp, reportid);

	strlcpy(out, dev, PATH_MAX);
	return 0;

nomatch:
	close(devfd);
	return 1;
}
