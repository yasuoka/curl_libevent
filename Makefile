LOCALBASE?=	/usr/local
PROG=		test

CFLAGS=		-I${LOCALBASE}/include
LDFLAGS=	-L${LOCALBASE}/lib

LDADD=		-lcurl -levent
SRCS=		curl_libevent.c test.c

NOMAN=		#

.include <bsd.prog.mk>
