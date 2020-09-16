AUTOMAKE_OPTIONS=foreign
SUBDIRS=src
DISTCLEANFILES=*~
AM_CPPFLAGS=-I$(builddir)

lsp:
	~/.local/bin/intercept-build make

format:
	clang-format -i src/util.cc src/fd.cc src/sim.cc src/approve.cc src/util.h src/fd.h

tidy:
	clang-tidy -header-filter='fd.h|util.h' -checks='*,-fuchsia-default-arguments,-llvm-header-guard,-readability-named-parameter,-readability-implicit-bool-conversion,-cppcoreguidelines-pro-bounds-pointer-arithmetic,-cppcoreguidelines-pro-type-union-access,-cppcoreguidelines-pro-type-reinterpret-cast,-android-cloexec-accept,-cppcoreguidelines-pro-bounds-array-to-pointer-decay,-llvm-header-guard,-google-readability-todo,-cert-err60-cpp' src/util.cc src/fd.cc src/sim.cc src/approve.cc
