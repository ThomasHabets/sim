AUTOMAKE_OPTIONS=foreign
SUBDIRS=src
DISTCLEANFILES=*~
AM_CPPFLAGS=-I$(builddir)

lsp:
	~/.local/bin/intercept-build make

format:
	clang-format -i src/util.cc src/fd.cc src/sim.cc src/approve.cc src/util.h src/fd.h
