#	$NetBSD: Makefile,v 1.3 2017/04/29 00:06:40 nonaka Exp $

PROG=	nvmectl
SRCS=	nvmectl.c identify.c logpage.c power.c
MAN=	nvmectl.8

DPADD+= ${LIBUTIL}
LDADD+= -lutil

.include <bsd.prog.mk>
