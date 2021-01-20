/*      $OpenBSD$ */

/*
 * Copyright (c) 2020 Marcus Glocker <mglocker@openbsd.org>
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

#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/fanio.h>

#define DEVICE	"/dev/fan0"

void	usage(void);
int	printall(int);
int	parse(char *);
void	getvalue0(char *);
void	getvalue1(char *);
void	setvalue(char *, int);

int	aflag, fd;

int
main(int argc, char *argv[])
{
	int ch, r;
	char *dev = NULL;

	while ((ch = getopt(argc, argv, "af:")) != -1) {
		switch (ch) {
		case 'a':
			aflag = 1;
			break;
		case 'f':
			dev = optarg;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (dev == NULL)
		dev = strdup(DEVICE);
	if (dev == NULL)
		err(1, "strdup");
	fd = open(dev, O_RDWR);
	if (fd == -1)
		err(1, "open");

	if (argc == 0 || aflag)
		r = printall(fd);

	if (aflag) {
		close(fd);
		return r;
	}

	for (; *argv != NULL; ++argv)
		if ((r = parse(*argv)) != 0)
			break;

	close(fd);
	return r;
}

void
usage(void)
{
	fprintf(stderr, "usage: fanctl [-a] [-f file]\n");
	exit(1);
}

int
printall(int fd)
{
	int i, r, nfans;
	struct fan_query_drv qd;
	struct fan_query_fan qf;

	r = ioctl(fd, FANIOC_QUERY_DRV, &qd);
	if (r == -1)
		err(1, "ioctl");
	if (qd.nfans < 1)
		return -1;

	printf("driver=%s\n", qd.id);

	for (i = 0; i < qd.nfans; i++) {
		memset(&qf, 0, sizeof(struct fan_query_fan));
		qf.idx = i;
		r = ioctl(fd, FANIOC_QUERY_FAN, &qf);
		if (r == -1)
			return -1;
		printf("fan%d.id=%s\n", i, qf.id);
		printf("fan%d.actual=%d RPM\n", i, qf.rpm_actual);
		printf("fan%d.min=%d RPM\n", i, qf.rpm_min);
		printf("fan%d.max=%d RPM\n", i, qf.rpm_max);
		printf("fan%d.safe=%d RPM\n", i, qf.rpm_safe);
		printf("fan%d.target=%d RPM\n", i, qf.rpm_target);
	}

	return 0;
}

int
parse(char *string)
{
	char *key, *val;
	const char *errstr;
	int valn;

	key = strdup(string);
	val = strchr(key, '=');
	if (val == NULL) {
		if (strchr(key, '.') == NULL)
			getvalue0(key);
		else
			getvalue1(key);
		return 0;
	}
	*val = '\0';
	val++;

	valn = strtonum(val, 0, 10000, &errstr);
	if (errstr != NULL) {
		warnx("%s: %s", key, errstr);
		free(key);
		return 1;
	}

	setvalue(key, valn);

	free(key);
	return 0;
}

void
getvalue0(char *key)
{
	struct fan_query_drv qd;
	char val[32];

	if (!strcmp(key, "driver")) {
		if (ioctl(fd, FANIOC_QUERY_DRV, &qd) == -1)
			err(1, "FANIOC_QUERY_DRV");
		strlcpy(val, qd.id, sizeof(val));
	}

	printf("%s=%s\n", key, val);
}

void
getvalue1(char *key)
{
	struct fan_query_fan qf;
	char *fan, *type;
	char fanno[3];
	const char *errstr;
	int fann, val;

	fan = strdup(key);
	type = strchr(fan, '.');
	if (type == NULL) {
		printf("what\n");
		return;
	}
	*type = '\0';
	type++;

	strlcpy(fanno, fan+3, sizeof(fanno));
	fann = strtonum(fanno, 0, 99, &errstr);

	qf.idx = fann;
	if (ioctl(fd, FANIOC_QUERY_FAN, &qf) == -1)
		err(1, "FANIOC_QUERY_FAN");

	if (!strcmp(type, "id")) {
		printf("%s=%s\n", key, qf.id);
		free(fan);
		return;
	}

	if (!strcmp(type, "act"))
		val = qf.rpm_actual;
	else if (!strcmp(type, "min"))
		val = qf.rpm_min;
	else if (!strcmp(type, "max"))
		val = qf.rpm_max;
	else if (!strcmp(type, "safe"))
		val = qf.rpm_safe;
	else if (!strcmp(type, "target"))
		val = qf.rpm_target;
	else {
		warnx("%s: unknown fan speed", type);
		free(fan);
		return;
	}

	printf("%s=%d\n", key, val);
	free(fan);
}

void
setvalue(char *key, int val)
{
	struct fan_query_fan qf;
	struct fan_set_rpm set_max, set_min;
	char *fan, *type;
	char fanno[3];
	const char *errstr;
	int fann;

	fan = strdup(key);
	type = strchr(fan, '.');
	if (type == NULL) {
		printf("what\n");
		return;
	}
	*type = '\0';
	type++;

	strlcpy(fanno, fan+3, sizeof(fanno));
	fann = strtonum(fanno, 0, 99, &errstr);

	qf.idx = fann;
	if (ioctl(fd, FANIOC_QUERY_FAN, &qf) == -1)
		err(1, "FANIOC_QUERY_FAN");

	if (!strcmp(type, "min")) {
		set_min.idx = fann;
		set_min.rpm = val;
		if (ioctl(fd, FANIOC_SET_MIN, &set_min) == -1)
			err(1, "FANIOC_SET_MIN");
		printf("%s: %d -> %d\n", key, qf.rpm_min, val);
	} else if (!strcmp(type, "max")) {
		set_max.idx = fann;
		set_max.rpm = val;
		if (ioctl(fd, FANIOC_SET_MAX, &set_max) == -1)
			err(1, "FANIOC_SET_MAX");
		printf("%s: %d -> %d\n", key, qf.rpm_max, val);
	} else {
		warnx("%s: unknown fan speed", type);
	}

	free(fan);
}
