bin_PROGRAMS=	hq
hq_SOURCES=	hq.cc picohttpparser/picohttpparser.c picolog/picolog.cc picoopt/picoopt.cc
if PICOEV_USE_KQUEUE
hq_SOURCES+=	picoev/picoev_kqueue.c
else
if PICOEV_USE_EPOLL
hq_SOURCES+=	picoev/picoev_epoll.c
else
hq_SOURCES+=	picoev/picoev_select.c
endif
endif
hq_CFLAGS=	$(AM_CFLAGS) -Wall
hq_CXXFLAGS=	$(AM_CFLAGS) -Wall
hq_LDADD=	-lpthread
