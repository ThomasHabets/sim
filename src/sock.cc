#include "sock.h"
#include <grp.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <vector>

namespace Sim {
SysError::SysError(const std::string& s) : std::runtime_error(s + ": " + strerror(errno))
{
}
SysError::SysError(const std::string& s, int e)
    : std::runtime_error(s + ": " + strerror(e))
{
}

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

std::string uid_to_username(uid_t uid)
{
    const struct passwd* pw = getpwuid(uid);
    if (!pw) {
        throw SysError("getpwuid(" + std::to_string(uid) + ")");
    }
    return pw->pw_name;
}

// Given group name, return gid.
gid_t group_to_gid(const std::string& group)
{
    struct group* gr = getgrnam(group.c_str());
    if (!gr) {
        throw SysError("getpwnam(" + group + ")");
    }
    return gr->gr_gid;
}

bool user_is_member(const std::string& user, const gid_t gid, const std::string& group)
{
    const gid_t admin_gid = group_to_gid(group);

    int groupcount = 0;
    getgrouplist(user.c_str(), gid, nullptr, &groupcount);
    std::vector<gid_t> groups(groupcount);
    const int rc = getgrouplist(user.c_str(), gid, groups.data(), &groupcount);
    if (groupcount != rc) {
        throw SysError("getgrouplist() returned " + std::to_string(rc) + ", want " +
                       std::to_string(groupcount));
    }
    for (const auto& g : groups) {
        if (g == admin_gid) {
            return true;
        }
    }
    return false;
}


} // namespace Sim
