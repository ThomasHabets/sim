AUTOMAKE_OPTIONS=foreign

bin_PROGRAMS=sim approve
sim_SOURCES=sim.cc \
fd.cc \
util.cc
nodist_sim_SOURCES=@builddir@/simproto.pb.cc @builddir@/simproto.pb.h

approve_SOURCES=approve.cc \
fd.cc \
util.cc
nodist_approve_SOURCES=@builddir@/simproto.pb.cc @builddir@simproto.pb.h

BUILT_SOURCES=simproto.pb.h
MOSTLYCLEANFILES=simproto.pb.cc simproto.pb.h
dist_noinst_DATA=simproto.proto

simproto.pb.cc simproto.pb.h: simproto.proto
	$(PROTOC) --proto_path=$(srcdir) --cpp_out=$(builddir) simproto.proto

install-exec-hook:
	echo "Setting suid bit"
	chmod 4711 $(DESTDIR)$(bindir)/sim
