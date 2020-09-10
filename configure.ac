# -*- Autoconf -*-

AC_PREREQ(2.61)
AC_INIT(sim, 0.01, thomas@habets.se)
AC_CANONICAL_SYSTEM
AC_CONFIG_SRCDIR([src/sim.cc])
AM_INIT_AUTOMAKE([foreign subdir-objects])
AM_MAINTAINER_MODE
AC_CONFIG_HEADER(src/config.h)

# Check for programs.
AC_PROG_CC
AC_PROG_CXX
AC_PROG_INSTALL
AC_PROG_MAKE_SET

# Check for libraries.
AC_LANG_CPLUSPLUS

AC_CHECK_PROG([PROTOC], [protoc], [protoc])
AS_IF([test "x${PROTOC}" == "x"],
    [AC_MSG_ERROR([ProtoBuf compiler "protoc" not found.])])

PKG_CHECK_MODULES(PROTOBUF, protobuf >= 2.4.0)
AC_SUBST(PROTOBUF_LIBS)
AC_SUBST(PROTOBUF_CFLAGS)
AC_SUBST(PROTOBUF_CXXFLAGS)
AC_SUBST(PROTOBUF_VERSION)
LIBS="$LIBS $PROTOBUF_LIBS"
CXXFLAGS="$CXXFLAGS $PROTOBUF_CXXFLAGS $PROTOBUF_CFLAGS"

# Check for header files.
AC_CHECK_HEADERS([\
signal.h \
sys/socket.h \
sys/types.h \
grp.h \
pwd.h \
unistd.h \
google/protobuf/stubs/logging.h \
google/protobuf/stubs/common.h \
])

AC_CHECK_FUNCS([clearenv])
AC_CHECK_MEMBERS([struct ucred.uid],[],[],[
#include<sys/types.h>
#include<sys/socket.h>
])
AC_CHECK_MEMBERS([struct sockpeercred.uid],[],[],[
#include<sys/types.h>
#include<sys/socket.h>
])

AC_TYPE_SIGNAL

CXXFLAGS="$CXXFLAGS -std=c++14"

# Output
AC_CONFIG_FILES([Makefile])
AC_OUTPUT

# Status
echo "
  $PACKAGE_NAME version $PACKAGE_VERSION
  Prefix.........: $prefix
  Debug Build....: $debug
  C++ Compiler...: $CXX $CXXFLAGS $CPPFLAGS
  Linker.........: $LD $LDFLAGS $LIBS
"
