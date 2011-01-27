AC_INIT(src/hq.cc)

AM_INIT_AUTOMAKE([hq], 0.0.1)
AC_DEFINE([HQ_VERSION],[0x00001],[hexadecimal version no])

AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_CXX
AC_PROG_CPP

OS=`uname -s | tr "[[:upper:]]" "[[:lower:]]"`

AM_CONDITIONAL([PICOEV_USE_EPOLL], [ test "$OS" = "linux" ])
AM_CONDITIONAL([PICOEV_USE_KQUEUE], [ test "$OS" = "darwin" -o "$OS" = "freebsd" ])

case "$OS" in
linux)
    AC_DEFINE(HQ_IS_LINUX, [1], [is linux])
    ;;
darwin | freebsd)
    AC_DEFINE(HQ_IS_BSD, [1], [is bsd])
    ;;
esac

AM_CONFIG_HEADER(src/config.h)
AC_OUTPUT(Makefile src/Makefile)
