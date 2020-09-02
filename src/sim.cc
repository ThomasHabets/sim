#include "simproto.pb.h"
#include "sock.h"

// 3rd party libraries
#include "google/protobuf/stubs/common.h"
#include "google/protobuf/text_format.h"

// C++
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <random>
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

class PushEUID
{
public:
    PushEUID(uid_t euid) : old_euid_(geteuid()) { seteuid(euid); }
    ~PushEUID() { seteuid(old_euid_); }

private:
    const uid_t old_euid_;
};

class Socket
{
public:
    Socket(std::string fn, uid_t suid, gid_t gid) : fn_(std::move(fn))
    {
        // Create socket.
        sock_ = socket(AF_UNIX, SOCK_SEQPACKET, 0);
        if (sock_ == -1) {
            throw SysError("socket");
        }

        // Bind.
        {
            PushEUID _(suid);
            struct sockaddr_un sa {
            };
            sa.sun_family = AF_UNIX;
            strncpy(sa.sun_path, fn_.c_str(), sizeof sa.sun_path);
            if (bind(sock_, reinterpret_cast<struct sockaddr*>(&sa), sizeof sa)) {
                const int e = errno;
                close();
                throw SysError("bind", e);
            }
            if (chown(fn_.c_str(), getuid(), gid)) {
                const int e = errno;
                close();
                throw SysError("fchmod", e);
            }
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
            std::clog << "sim: Failed to delete socket <" << fn_
                      << ">: " << strerror(errno) << std::endl;
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
    Checker(const std::string& socks_dir,
            uid_t suid,
            gid_t approver_gid,
            std::vector<std::string> args)
        : approver_gid_(approver_gid),
          sock_(make_sock_filename(socks_dir), suid, approver_gid),
          args_(std::move(args))
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
            if (fd.get_uid() == getuid()) {
                std::cerr << "sim: Can't approve our own command\n";
                continue;
            }

            if (fd.get_uid() == getuid()) {
                std::cerr << "sim: Can't approve our own command\n";
                continue;
            }

            fd.write(data);
            simproto::ApproveResponse resp;
            if (!resp.ParseFromString(fd.read())) {
                std::clog << "sim: Failed to parse approval request\n";
                continue;
            }
            const auto uid = fd.get_uid();
            auto user = uid_to_username(uid);
            if (resp.approved()) {
                std::clog << "sim: Approved by <" << user << "> (" << uid << ")\n";
                break;
            } else {
                std::clog << "sim: Rejected by <" << user << "> (" << uid << ")\n";
            }
        }
    }

private:
    static std::string make_sock_filename(const std::string& dir)
    {
        const std::string alphabet("0123456789ABCDEF");

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> rnd(0, alphabet.size() - 1);

        std::vector<char> data(32); // 128 bits.
        std::generate(std::begin(data), std::end(data), [&alphabet, &rnd, &gen] {
            return alphabet[rnd(gen)];
        });
        return dir + "/" + std::string(std::begin(data), std::end(data));
    }

    gid_t approver_gid_;
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

    // Save the effective user for later when we re-claim root.
    const uid_t nuid = geteuid();

    seteuid(getuid());

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

    gid_t approve_gid = -1;
    {
        struct group* gr = getgrnam(config.approve_group().c_str());
        if (!gr) {
            throw SysError("getpwnam(" + config.approve_group() + ")");
        }
        approve_gid = gr->gr_gid;
    }

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
    std::cerr << "sim: Waiting for MPA approval...\n";
    {
        Checker check(
            config.sock_dir(), nuid, approve_gid, args_to_vector(argc - 1, &argv[1]));
        check.check();
    }
    std::cerr << "sim: command approved!\n";

    // Become fully root.
    std::clog << "sim: Setting UID " << nuid << std::endl;
    if (setresuid(nuid, nuid, nuid)) {
        throw SysError("setresuid");
    }
    const gid_t ngid = get_primary_group(nuid);
    std::clog << "sim: Setting GID " << ngid << std::endl;
    if (setresgid(ngid, ngid, ngid)) {
        throw SysError("setresgid");
    }

    // This only works for root.
    if (setgroups(0, nullptr)) {
        // throw SysError("setgroups(0, nullptr)");
        std::clog << "sim: setgroups(0, nullptr) failed: " << strerror(errno)
                  << std::endl;
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
