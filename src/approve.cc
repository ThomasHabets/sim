// Project
#include "simproto.pb.h"
#include "sock.h"

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
#include <pwd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

class Defer
{
public:
    Defer(std::function<void()> func) : func_(func) {}

private:
    std::function<void()> func_;
};

FD connect(const std::string& fn)
{
    // Create socket.
    int sock = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (sock == -1) {
        throw SysError("socket");
    }

    // connect.
    struct sockaddr_un sa {
    };
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, fn.c_str(), sizeof sa.sun_path);
    if (::connect(sock, reinterpret_cast<struct sockaddr*>(&sa), sizeof sa)) {
        const int e = errno;
        close(sock);
        throw SysError("connect", e);
    }

    return FD(sock);
}

class Socket
{
public:
    Socket(const std::string& fn) : fd_(connect(fn)), fn_(fn) {}
    ~Socket() {}
    FD& fd() { return fd_; }

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

int main()
{
    // Load config.
    simproto::SimConfig config;
    {
        std::ifstream f(config_file);
        const std::string str((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
        if (!google::protobuf::TextFormat::ParseFromString(str, &config)) {
            throw std::runtime_error("error parsing config");
        }
    }
    const auto socks = list_dir(config.sock_dir());
    if (socks.empty()) {
        std::cerr << "Nothing to approve\n";
        return 1;
    }
    for (const auto& fn : socks) {
        std::cerr << "Picking up " << fn << std::endl;
        Socket sock(config.sock_dir() + "/" + fn);

        simproto::ApproveRequest req;
        if (!req.ParseFromString(sock.fd().read())) {
            std::cerr << "Failed to parse approve request proto\n";
            continue;
        }

        // Print request.
        {
            const auto uid = sock.fd().get_uid();
            std::cerr << "From user <" << uid_to_username(uid) << "> (" << uid << ")\n";
	    // TODO: check that they are in the admin group.
        }

        std::string s;
        if (!google::protobuf::TextFormat::PrintToString(req, &s)) {
            std::cerr << "Failed to print ASCII version of proto\n";
            continue;
        }
        const std::string bar = "------------------";
        std::cout << bar << std::endl << s << bar << std::endl;

        simproto::ApproveResponse resp;
        for (bool valid = false, prompt = true; !valid;) {
            if (prompt) {
                std::cout << "Approve? [y/n] " << std::flush;
            }
            prompt = true;

            const auto answer = getchar();
            switch (answer) {
            case '\n':
            case '\r':
                prompt = false;
                continue;
            case 'Y':
            case 'y':
                resp.set_approved(true);
                valid = true;
                break;
            case 'N':
            case 'n':
                resp.set_approved(false);
                valid = true;
                break;
            }
        }
        std::string resps;
        if (!resp.SerializeToString(&resps)) {
            std::cerr << "Failed to serialize approve response proto\n";
            continue;
        }
        sock.fd().write(resps);
    }
}
