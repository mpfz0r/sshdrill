#	$OpenBSD: Makefile,v 1.3 1997/09/21 11:50:42 deraadt Exp $

PROG=	sshwrap
CFLAGS+= -Wall
CFLAGS+= -Wstrict-prototypes -Wmissing-prototypes
CFLAGS+= -Wmissing-declarations
CFLAGS+= -Wshadow -Wpointer-arith -Wcast-qual
CFLAGS+= -Wsign-compare
LDADD=	-lutil
DPADD=	${LIBUTIL}
NOMAN= true

CPPFLAGS+=-DHAVE_UTIL_H
.include <bsd.prog.mk>
