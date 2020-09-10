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
#include "fd.h"

// Project
#include "util.h"

// C++
#include <vector>

// POSIX
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef HAVE_STRUCT_SOCKPEERCRED_UID
typedef struct sockpeercred ucred_t;
#elif defined(HAVE_STRUCT_UCRED_UID)
typedef struct ucred ucred_t;
#else
#error "Could not find any SO_PEERCRED struct"
#endif

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
    ucred_t ucred{};
    socklen_t len = sizeof(ucred_t);
    if (getsockopt(fd_, SOL_SOCKET, SO_PEERCRED, &ucred, &len) == -1) {
        throw SysError("getsockopt(,,SO_PEERCRED)");
    }
    return ucred.uid;
}

gid_t FD::get_gid() const
{
    ucred_t ucred{};
    socklen_t len = sizeof(ucred_t);
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
