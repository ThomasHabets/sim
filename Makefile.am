AUTOMAKE_OPTIONS=foreign
DISTCLEANFILES=*~
bin_PROGRAMS=sim approve

sim_SOURCES=src/sim.cc \
src/sock.cc \
src/simproto.pb.cc

approve_SOURCES=src/approve.cc \
src/sock.cc \
src/simproto.pb.cc


protos:
	protoc --cpp_out . src/simproto.proto

format:
	clang-format -i src/sock.cc src/sim.cc src/approve.cc src/sock.h
