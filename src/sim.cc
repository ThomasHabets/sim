#include "fd.h"
#include "simproto.pb.h"
#include "util.h"

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

// POSIX
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace Sim {
namespace {
volatile sig_atomic_t sigint = 0;

void sighandler(int) { sigint = 1; }

// Temporarily set EUID RAII-style.
class PushEUID
{
public:
    PushEUID(uid_t euid) : old_euid_(geteuid()) { seteuid(euid); }

    // No copy or move.
    PushEUID(const PushEUID&) = delete;
    PushEUID(PushEUID&&) = delete;
    PushEUID& operator=(const PushEUID&) = delete;
    PushEUID& operator=(PushEUID&&) = delete;

    ~PushEUID() { seteuid(old_euid_); }

private:
    const uid_t old_euid_;
};

// Make a random file name.
std::string make_sock_filename()
{
    const std::string alphabet("0123456789ABCDEF");

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> rnd(0, alphabet.size() - 1);

    std::vector<char> data(32); // 128 bits.
    std::generate(std::begin(data), std::end(data), [&alphabet, &rnd, &gen] {
        return alphabet[rnd(gen)];
    });
    return std::string(std::begin(data), std::end(data));
}

// Turn argc/argv into vector of strings.
std::vector<std::string> args_to_vector(int argc, char** argv)
{
    std::vector<std::string> ret;
    for (int c = 0; c < argc; c++) {
        ret.push_back(argv[c]);
    }
    return ret;
}

// Get primary group of user.
gid_t get_primary_group(uid_t uid)
{
    const struct passwd* pw = getpwuid(uid);
    if (pw == nullptr) {
        throw SysError("getpwuid");
    }
    return pw->pw_gid;
}

// Server side unix socket.
class SimSocket
{
public:
    SimSocket(std::string fn, uid_t suid, gid_t gid);

    // No copy or move.
    SimSocket(const SimSocket&) = delete;
    SimSocket(SimSocket&&) = delete;
    SimSocket& operator=(const SimSocket&) = delete;
    SimSocket& operator=(SimSocket&&) = delete;

    ~SimSocket();

    void close();
    FD accept();

private:
    int sock_;
    const uid_t suid_;
    const std::string fn_;
};

SimSocket::SimSocket(std::string fn, uid_t suid, gid_t gid)
    : suid_(suid), fn_(std::move(fn))
{
    // Create socket.
    sock_ = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (sock_ == -1) {
        throw SysError("socket");
    }
    Defer defer([&] { close(); });

    // Bind socket.
    {
        PushEUID _(suid);
        struct sockaddr_un sa {
        };
        sa.sun_family = AF_UNIX;
        strncpy(sa.sun_path, fn_.c_str(), sizeof sa.sun_path);
        if (bind(sock_, reinterpret_cast<struct sockaddr*>(&sa), sizeof sa)) {
            throw SysError("bind");
        }
        if (chown(fn_.c_str(), getuid(), gid)) {
            throw SysError("fchmod");
        }
    }
    if (chmod(fn_.c_str(), 0660)) {
        throw SysError("fchmod");
    }

    // Listen.
    if (listen(sock_, 10)) {
        throw SysError("listen");
    }
    defer.defuse();
}

void SimSocket::close()
{
    if (sock_ > 0) {
        ::close(sock_);
        sock_ = -1;
    }
}

SimSocket::~SimSocket()
{
    close();
    PushEUID _(suid_);
    if (unlink(fn_.c_str())) {
        std::clog << "sim: Failed to delete socket <" << fn_ << ">: " << strerror(errno)
                  << std::endl;
    }
}

FD SimSocket::accept()
{
    struct sockaddr_storage sa;
    socklen_t len = sizeof sa;
    int ret = ::accept(sock_, reinterpret_cast<struct sockaddr*>(&sa), &len);
    if (ret == -1) {
        throw SysError("accept");
    }
    return FD(ret);
}

class Checker
{
public:
    Checker(const std::string& socks_dir,
            uid_t suid,
            std::string approver,
            std::vector<std::string> args);

    void set_justification(std::string j);

    // Only returns if check approves action. Otherwise loops forever or
    // throws.
    void check();

private:
    const std::string approver_group_;
    const gid_t approver_gid_;
    SimSocket sock_;
    std::string justification_;
    const std::vector<std::string> args_;
};

Checker::Checker(const std::string& socks_dir,
                 uid_t suid,
                 std::string approver,
                 std::vector<std::string> args)
    : approver_group_(std::move(approver)),
      approver_gid_(group_to_gid(approver_group_)),
      sock_(socks_dir + "/" + make_sock_filename(), suid, approver_gid_),
      args_(std::move(args))
{
}

void Checker::set_justification(std::string j) { justification_ = std::move(j); }

void Checker::check()
{
    simproto::ApproveRequest req;

    // Construct proto.
    auto cmd = req.mutable_command();
    cmd->set_command(args_[0]);
    for (const auto& a : args_) {
        *cmd->add_args() = a;
    }
    if (!justification_.empty()) {
        req.set_justification(justification_);
    }

    // Serialize.
    std::string data;
    if (!req.SerializeToString(&data)) {
        throw std::runtime_error("failed to serialize approval request");
    }

    // Try to get it approved.
    for (;;) {
        auto fd = sock_.accept();
        const auto uid = fd.get_uid();
        if (uid == getuid()) {
            std::cerr << "sim: Can't approve our own command\n";
            continue;
        }

        // Check that they are an approver.
        {
            const auto user = uid_to_username(uid);
            const auto gid = fd.get_gid();
            if (!user_is_member(user, gid, approver_group_)) {
                throw std::runtime_error("user <" + user +
                                         "> is not part of approver group <" +
                                         approver_group_ + ">");
            }
        }

        fd.write(data);
        simproto::ApproveResponse resp;
        if (!resp.ParseFromString(fd.read())) {
            std::clog << "sim: Failed to parse approval request\n";
            continue;
        }
        auto user = uid_to_username(uid);
        if (resp.approved()) {
            std::clog << "sim: Approved by <" << user << "> (" << uid << ")\n";
            break;
        } else {
            std::clog << "sim: Rejected by <" << user << "> (" << uid << ")\n";
        }
    }
}

void usage(const char* av0, int err)
{
    printf("%s: Usage TODO\n", av0);
    exit(err);
}

} // namespace

int mainwrap(int argc, char** argv)
{
    // TODO: optionally allow logs.
    ::google::protobuf::LogSilencer silence;

    // Save the effective user for later when we re-claim root.
    const uid_t nuid = geteuid();

    if (-1 == seteuid(getuid())) {
        throw SysError("seteuid(getuid)");
    }

    // Option parsing.
    std::string justification;
    {
        int opt;
        while ((opt = getopt(argc, argv, "+hj:")) != -1) {
            switch (opt) {
            case 'h':
                usage(argv[0], EXIT_SUCCESS);
                break;
            case 'j':
                justification = optarg;
                break;
            default: /* '?' */
                usage(argv[0], EXIT_FAILURE);
            }
        }
    }


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

    const gid_t admin_gid = group_to_gid(config.admin_group());

    // Check that we are admin.
    {
        const int count = getgroups(0, nullptr);
        if (count == -1) {
            throw SysError("getgroups(0, nullptr)");
        }
        std::vector<gid_t> gs(count);
        if (count != getgroups(gs.size(), gs.data())) {
            throw SysError("getgroups(all)");
        }
        if (getuid() && std::count(std::begin(gs), std::end(gs), admin_gid) == 0) {
            // NOTE: Deliberately not saying which group is the admin group.
            throw std::runtime_error("user <" + uid_to_username(getuid()) +
                                     "> is not in admin group");
        }
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
        Checker check(config.sock_dir(),
                      nuid,
                      config.approve_group(),
                      args_to_vector(argc - optind, &argv[optind]));
        if (!justification.empty()) {
            check.set_justification(justification);
        }
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
    execvp(argv[optind], &argv[optind]);
    perror("execv");
    return 1;
}
} // namespace Sim

int main(int argc, char** argv)
{
    try {
        return Sim::mainwrap(argc, argv);
    } catch (const std::exception& e) {
        if (Sim::sigint) {
            std::cerr << "Aborted\n";
        } else {
            std::cerr << e.what() << std::endl;
        }
        return 1;
    }
}
