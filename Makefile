
PROG=	sshdrill
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
