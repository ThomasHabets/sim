// Self
#include "fd.h"

// Project
#include "util.h"

// C++
#include <vector>

// POSIX
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace Sim {
FD::FD(int fd) : fd_(fd) {}
FD::FD(FD&& rhs) : fd_(rhs.fd_) { rhs.fd_ = -1; }

void FD::close()
{
    if (fd_ > 0) {
        ::close(fd_);
        fd_ = -1;
    }
}


FD::~FD() { close(); }

uid_t FD::get_uid() const
{
    struct ucred ucred {
    };
    socklen_t len = sizeof(struct ucred);
    if (getsockopt(fd_, SOL_SOCKET, SO_PEERCRED, &ucred, &len) == -1) {
        throw SysError("getsockopt(,,SO_PEERCRED)");
    }
    return ucred.uid;
}

gid_t FD::get_gid() const
{
    struct ucred ucred {
    };
    socklen_t len = sizeof(struct ucred);
    if (getsockopt(fd_, SOL_SOCKET, SO_PEERCRED, &ucred, &len) == -1) {
        throw SysError("getsockopt(,,SO_PEERCRED)");
    }
    return ucred.gid;
}

void FD::write(const std::string& s)
{
    const auto rc = ::write(fd_, s.data(), s.size());
    if (rc == -1) {
        throw SysError("write");
    }
    if (s.size() != static_cast<size_t>(rc)) {
        throw std::runtime_error("short write");
    }
}
std::string FD::read()
{
    std::vector<char> buf(1024);
    const auto rc = ::read(fd_, buf.data(), buf.size());
    if (rc == -1) {
        throw SysError("read");
    }
    return std::string(&buf[0], &buf[rc]);
}
} // namespace Sim
