AUTOMAKE_OPTIONS=foreign
DISTCLEANFILES=*~
bin_PROGRAMS=sim approve

sim_SOURCES=src/sim.cc \
src/fd.cc \
src/util.cc \
$(builddir)/src/simproto.pb.cc

approve_SOURCES=src/approve.cc \
src/fd.cc \
src/util.cc \
$(builddir)/src/simproto.pb.cc

lsp:
	~/.local/bin/intercept-build make

%.pb.cc %.pb.h: %.proto
	$(PROTOC) --proto_path=$(srcdir) --cpp_out=$(builddir) $^

format:
	clang-format -i src/util.cc src/fd.cc src/sim.cc src/approve.cc src/util.h src/fd.h

install-exec-hook:
	echo "Setting suid bit"
	chmod 4711 $(DESTDIR)$(bindir)/sim

dist_noinst_DATA=src/simproto.proto
