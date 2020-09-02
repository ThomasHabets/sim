#include "simproto.pb.h"
#include "sock.h"

// 3rd party libraries
#include "google/protobuf/stubs/common.h"

// C++
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// C
#include <unistd.h>

// UNIX
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>

namespace {
volatile sig_atomic_t sigint = 0;
void sighandler(int) { sigint = 1; }
} // namespace


class Socket
{
public:
    Socket(std::string fn) : fn_(std::move(fn))
    {
        // Create socket.
        sock_ = socket(AF_UNIX, SOCK_SEQPACKET, 0);
        if (sock_ == -1) {
            throw SysError("socket");
        }

        // Bind.
        struct sockaddr_un sa {
        };
        sa.sun_family = AF_UNIX;
        strncpy(sa.sun_path, fn_.c_str(), sizeof sa.sun_path);
        if (bind(sock_, reinterpret_cast<struct sockaddr*>(&sa), sizeof sa)) {
            const int e = errno;
            close();
            throw SysError("bind", e);
        }
        if (chmod(fn_.c_str(), 0660)) {
            const int e = errno;
            close();
            throw SysError("fchmod", e);
        }

        // Listen.
        if (listen(sock_, 10)) {
            const int e = errno;
            close();
            throw SysError("listen", e);
        }
    }
    void close()
    {
        if (sock_ > 0) {
            ::close(sock_);
            sock_ = -1;
        }
    }
    ~Socket()
    {
        close();
        if (unlink(fn_.c_str())) {
            std::clog << "Failed to delete socket <" << fn_ << ">: " << strerror(errno)
                      << std::endl;
        }
    }
    FD accept()
    {
        struct sockaddr_storage sa;
        socklen_t len = sizeof sa;
        int ret = ::accept(sock_, reinterpret_cast<struct sockaddr*>(&sa), &len);
        if (ret == -1) {
            throw SysError("accept");
        }
        return FD(ret);
    }

private:
    int sock_;
    const std::string fn_;
};

class Checker
{
public:
    Checker(const std::string& socks_dir, std::vector<std::string> args)
        : sock_(make_sock_filename(socks_dir)), args_(std::move(args))
    {
    }

    void check()
    {
        simproto::ApproveRequest req;

        // Construct proto.
        auto cmd = req.mutable_command();
        cmd->set_command(args_[0]);
        for (const auto& a : args_) {
            *cmd->add_args() = a;
        }

        // Serialize.
        std::string data;
        if (!req.SerializeToString(&data)) {
            throw std::runtime_error("failed to serialize approval request");
        }

        // Try to get it approved.
        for (;;) {
            auto fd = sock_.accept();
            if (fd.getUID() == getuid()) {
                std::cerr << "Can't approve our own command\n";
                continue;
            }

            fd.write(data);
            simproto::ApproveResponse resp;
            if (!resp.ParseFromString(fd.read())) {
                std::clog << "Failed to parse approval request\n";
                continue;
            }
            if (resp.approved()) {
                std::clog << "Approved by UID " << fd.getUID() << std::endl;
                break;
            } else {
                std::clog << "Rejected by UID " << fd.getUID() << std::endl;
            }
        }
    }

private:
    static std::string make_sock_filename(const std::string& dir)
    {
        // TODO: UUID.
        return dir + "/foo";
    }

    Socket sock_;
    const std::vector<std::string> args_;
};

std::vector<std::string> args_to_vector(int argc, char** argv)
{
    std::vector<std::string> ret;
    for (int c = 0; c < argc; c++) {
        ret.push_back(argv[c]);
    }
    return ret;
}

gid_t get_primary_group(uid_t uid)
{
    const struct passwd* pw = getpwuid(uid);
    if (pw == nullptr) {
        throw SysError("getpwuid");
    }
    return pw->pw_gid;
}

int mainwrap(int argc, char** argv)
{
    // TODO: optionally allow logs.
    ::google::protobuf::LogSilencer silence;

    std::string socks_dir = "socks";
    if (argc < 2) {
        std::cerr << "Usage blah\n";
        return 1;
    }

    struct sigaction sigact {
    };
    sigact.sa_handler = sighandler;

    if (sigaction(SIGINT, &sigact, nullptr)) {
        throw SysError("sigaction");
    }
    std::cerr << "Waiting for MPA approval...\n";
    {
        Checker check(socks_dir, args_to_vector(argc - 1, &argv[1]));
        check.check();
    }
    std::cerr << "... approved!\n";

    // Become fully root.
    const uid_t nuid = geteuid();
    std::clog << "Setting UID " << nuid << std::endl;
    if (setresuid(nuid, nuid, nuid)) {
        throw SysError("setresuid");
    }
    const gid_t ngid = get_primary_group(nuid);
    std::clog << "Setting GID " << ngid << std::endl;
    if (setresgid(ngid, ngid, ngid)) {
        throw SysError("setresgid");
    }

    // This only works for root.
    if (setgroups(0, nullptr)) {
        // throw SysError("setgroups(0, nullptr)");
        std::clog << "setgroups(0, nullptr) failed: " << strerror(errno) << std::endl;
    }

    // Clear environment.
    if (clearenv()) {
        throw SysError("clearenv");
    }

    // Execute command.
    execvp(argv[1], &argv[1]);
    perror("execv");
    return 1;
}

int main(int argc, char** argv)
{
    try {
        return mainwrap(argc, argv);
    } catch (const std::exception& e) {
        if (sigint) {
            std::cerr << "Aborted\n";
        } else {
            std::cerr << e.what() << std::endl;
        }
        return 1;
    }
}
