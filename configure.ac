# -*- Autoconf -*-

AC_PREREQ(2.61)
AC_INIT(sim, 0.01, thomas@habets.se)
AC_CANONICAL_SYSTEM
AC_CONFIG_SRCDIR([src/sim.cc])
AM_INIT_AUTOMAKE([foreign subdir-objects])
AM_MAINTAINER_MODE
AC_CONFIG_HEADER(src/config.h)

# Check for programs.
AC_PROG_CXX
AC_PROG_INSTALL
AC_PROG_MAKE_SET

# Check for libraries.
AC_LANG_CPLUSPLUS
AC_MSG_CHECKING([for protobuf])
LIBS="$LIBS -lprotobuf"
AC_TRY_LINK([
#include <google/protobuf/stubs/common.h>
#include <google/protobuf/stubs/logging.h>
],
        [::google::protobuf::LogSilencer object;],
        has_protobuf=1,
        has_protobuf=0)
if test $has_protobuf = 0; then
  AC_MSG_RESULT([no])
  AC_ERROR([Could not find protobuf libraries])
else
  AC_MSG_RESULT([yes])
fi

# Check for header files.
AC_CHECK_HEADERS([\
signal.h \
sys/socket.h \
sys/types.h \
grp.h \
pwd.h \
unistd.h])

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
