/*
 *    Copyright 2020 Google LLC
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        https://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */
// Self
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "util.h"

// C++
#include <cerrno>
#include <cstring>
#include <iostream>
#include <vector>

// POSIX
#include <grp.h>
#include <pwd.h>
#include <unistd.h>

namespace Sim {
namespace {
constexpr int max_group_count = 1000;
} // namespace

SysError::SysError(const std::string& s) : std::runtime_error(s + ": " + strerror(errno))
{
}

// Temporarily set EUID RAII-style.
PushEUID::PushEUID(uid_t euid) : old_euid_(geteuid())
{
    if (seteuid(euid)) {
        throw SysError("PushEUID: seteuid(" + std::to_string(euid) + ")");
    }
}

PushEUID::~PushEUID()
{
    if (seteuid(old_euid_)) {
        std::cerr << "~PushEUID: seteuid(" << old_euid_ << ")\n";
        std::terminate();
    }
}

std::string uid_to_username(uid_t uid)
{
    const struct passwd* pw = getpwuid(uid);
    if (pw == nullptr) {
        throw SysError("getpwuid(" + std::to_string(uid) + ")");
    }
    return pw->pw_name;
}

// Given group name, return gid.
gid_t group_to_gid(const std::string& group)
{
    struct group* gr = getgrnam(group.c_str());
    if (gr == nullptr) {
        throw SysError("getpwnam(" + group + ")");
    }
    return gr->gr_gid;
}

bool user_is_member(const std::string& user, const gid_t gid, const std::string& group)
{
    const gid_t admin_gid = group_to_gid(group);

    int groupcount = 0;
    bool check_group_count = true;

    if (false) {
        // This doesn't work on OpenBSD, since only(?) Linux gives us
        // the info we want.
        getgrouplist(user.c_str(), gid, nullptr, &groupcount);
    } else {
        groupcount = max_group_count;
        check_group_count = false;
    }

    std::vector<gid_t> groups(groupcount);
    const int rc = getgrouplist(user.c_str(), gid, groups.data(), &groupcount);
    if (rc < 0) {
        throw SysError("getgrouplist()");
    }
    if (check_group_count) {
        if (groupcount != rc) {
            throw SysError("getgrouplist() returned " + std::to_string(rc) + ", want " +
                           std::to_string(groupcount));
        }
    }
    for (const auto& g : groups) {
        if (g == admin_gid) {
            return true;
        }
    }
    return false;
}


} // namespace Sim
