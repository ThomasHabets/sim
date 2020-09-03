// Self
#include "util.h"

// C++
#include <cerrno>
#include <cstring>
#include <vector>

// POSIX
#include <grp.h>
#include <pwd.h>

namespace Sim {
SysError::SysError(const std::string& s) : std::runtime_error(s + ": " + strerror(errno))
{
}
SysError::SysError(const std::string& s, int e)
    : std::runtime_error(s + ": " + strerror(e))
{
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
