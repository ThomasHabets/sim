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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
// Project
#include "fd.h"
#include "simproto.pb.h"
#include "util.h"

// Libraries
#include "google/protobuf/text_format.h"

// C++
#include <cerrno>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

// POSIX
#include <dirent.h>
#include <grp.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace Sim {
FD connect(const std::string& fn)
{
    // Create socket.
    int sock = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (sock == -1) {
        throw SysError("socket");
    }
    Defer defer([&] { ::close(sock); });

    // connect.
    struct sockaddr_un sa {
    };
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, fn.c_str(), sizeof sa.sun_path);
    if (::connect(sock, reinterpret_cast<struct sockaddr*>(&sa), sizeof sa)) {
        throw SysError("connect");
    }

    FD fd(sock);
    defer.defuse();
    return std::move(fd);
}


class ApproveSocket
{
public:
    ApproveSocket(const std::string& fn) : fd_(connect(fn)), fn_(fn) {}

    // No copy or move.
    ApproveSocket(const ApproveSocket&) = delete;
    ApproveSocket(ApproveSocket&&) = delete;
    ApproveSocket& operator=(const ApproveSocket&) = delete;
    ApproveSocket& operator=(ApproveSocket&&) = delete;

    FD& fd() noexcept { return fd_; }

private:
    FD fd_;
    const std::string fn_;
};

std::vector<std::string> list_dir(const std::string& d)
{
    // TODO(C++17): https://en.cppreference.com/w/cpp/filesystem/directory_iterator
    DIR* dir = opendir(d.c_str());
    if (dir == nullptr) {
        throw SysError("opendir");
    }
    Defer _([&dir] { closedir(dir); });
    std::vector<std::string> ret;
    for (;;) {
        errno = 0;
        struct dirent* ent = readdir(dir);
        if (ent == nullptr) {
            if (errno == 0) {
                break;
            }
            throw SysError("readdir");
        }
        if (ent->d_type != DT_SOCK) {
            continue;
        }
        ret.push_back(ent->d_name);
    }
    return ret;
}

void handle_request(const simproto::SimConfig& config, const std::string fn)
{
    std::cerr << "Picking up " << fn << std::endl;
    ApproveSocket sock(config.sock_dir() + "/" + fn);

    simproto::ApproveRequest req;
    if (!req.ParseFromString(sock.fd().read())) {
        throw std::runtime_error("failed to parse approve request proto");
    }

    // Check that other side is part of admin group.
    {
        const auto gid = sock.fd().get_gid();
        const auto uid = sock.fd().get_uid();
        const auto user = uid_to_username(uid);
        if (!user_is_member(user, gid, config.admin_group())) {
            throw std::runtime_error("user <" + user + "> is not part of admin group <" +
                                     config.admin_group() + ">");
        }
        std::cerr << "From user <" << user << "> (" << uid << ")\n";
    }

    // Print request.
    {
        std::string s;
        if (!google::protobuf::TextFormat::PrintToString(req, &s)) {
            throw std::runtime_error("failed to print ASCII version of proto");
        }
        const std::string bar = "------------------";
        std::cout << bar << std::endl << s << bar << std::endl;
    }

    // Check with user if we should approve.
    simproto::ApproveResponse resp;
    for (bool valid = false, prompt = true; !valid;) {
        if (prompt) {
            std::cout << "Approve? [y]es / [n]o / [c]omment> " << std::flush;
        }
        prompt = true;

        const auto answer = getchar();
        switch (tolower(answer)) {
        case '\n':
        case '\r':
            prompt = false;
            continue;
        case 'y':
            resp.set_approved(true);
            valid = true;
            break;
        case 'n':
            resp.set_approved(false);
            valid = true;
            break;
        case 'c':
            getchar(); // Flush the newline.
            std::cout << "Enter comment and press enter:\n";
            const auto comment = [] {
                std::string ret;
                std::getline(std::cin, ret);
                return ret;
            }();
            resp.set_approved(false);
            resp.set_comment(comment);
            valid = true;
            break;
        }
    }

    // Send reply.
    std::string resps;
    if (!resp.SerializeToString(&resps)) {
        throw std::runtime_error("failed to serialize approve response proto");
    }
    sock.fd().write(resps);
}

void usage(const char* av0, int err)
{
    printf("%s: Usage [ -h ]\n", av0);
    exit(err);
}

int mainwrap(int argc, char** argv)
{
    // Parse options.
    {
        int opt;
        while ((opt = getopt(argc, argv, "h")) != -1) {
            switch (opt) {
            case 'h':
                usage(argv[0], EXIT_SUCCESS);
                break;
            default: /* '?' */
                usage(argv[0], EXIT_FAILURE);
            }
        }
    }
    if (argc != optind) {
        throw std::runtime_error("Trailing args on command line");
    }

    // Load config.
    simproto::SimConfig config;
    {
        std::ifstream f(config_file);
        const std::string str((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
        if (!google::protobuf::TextFormat::ParseFromString(str, &config)) {
            throw std::runtime_error("error parsing config " + std::string(config_file));
        }
    }

    // Find list of things to approve.
    const auto socks = list_dir(config.sock_dir());
    if (socks.empty()) {
        std::cerr << "Nothing to approve\n";
        return 1;
    }

    // Loop over them and approve them.
    for (const auto& fn : socks) {
        try {
            handle_request(config, fn);
        } catch (const std::exception& e) {
            std::cerr << "Failed to handle " << fn << ": " << e.what() << std::endl;
        }
    }
    return EXIT_SUCCESS;
}

} // namespace Sim

int main(int argc, char** argv)
{
    try {
        return Sim::mainwrap(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
