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
#include "fd.h"
#include "simproto.pb.h"
#include "util.h"

// 3rd party libraries
#ifdef HAVE_GOOGLE_PROTOBUF_STUBS_COMMON_H
#include "google/protobuf/stubs/common.h"
#endif
#ifdef HAVE_GOOGLE_PROTOBUF_STUBS_LOGGING_H
#include "google/protobuf/stubs/logging.h"
#endif
#include "google/protobuf/text_format.h"

// C++
#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <random>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

// POSIX
#include <grp.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <unistd.h>

extern char** environ;

namespace Sim {

int do_edit(uid_t uid, gid_t gid, const std::string& fn);


namespace {
constexpr int max_backlog = 10;
constexpr mode_t sock_dir_mode = 0755;
constexpr mode_t sock_file_mode = 0660;

volatile sig_atomic_t sigint = 0;

void sighandler(int) { sigint = 1; }

#ifndef HAVE_CLEARENV
int clearenv()
{
    *environ = nullptr;
    return 0;
}
#endif

// Make a random file name.
[[nodiscard]] std::string make_sock_filename()
{
    const std::string alphabet("0123456789ABCDEF");

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> rnd(0, alphabet.size() - 1);

    constexpr int sock_filename_len = 32; // 32*4=128 bits.
    std::vector<char> data(sock_filename_len);
    std::generate(std::begin(data), std::end(data), [&alphabet, &rnd, &gen] {
        return alphabet[rnd(gen)];
    });
    return std::string(std::begin(data), std::end(data));
}

// Turn argc/argv into vector of strings.
[[nodiscard]] std::vector<std::string> args_to_vector(int argc, char** argv)
{
    std::vector<std::string> ret;
    ret.reserve(argc);
    for (int c = 0; c < argc; c++) {
        ret.emplace_back(argv[c]);
    }
    return ret;
}

// Get primary group of user.
[[nodiscard]] gid_t get_primary_group(uid_t uid)
{
    const struct passwd* pw = getpwuid(uid);
    if (pw == nullptr) {
        throw SysError("getpwuid(" + std::to_string(uid) + ")");
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
    SimSocket(SimSocket&&) noexcept;
    SimSocket& operator=(const SimSocket&) = delete;
    SimSocket& operator=(SimSocket&&) noexcept = delete;

    ~SimSocket();

    void close();
    [[nodiscard]] FD accept();

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
        strncpy(static_cast<char*>(sa.sun_path), fn_.c_str(), sizeof sa.sun_path);
        if (bind(sock_, reinterpret_cast<struct sockaddr*>(&sa), sizeof sa)) {
            throw SysError("bind(" + fn_ + ")");
        }
        if (chown(fn_.c_str(), getuid(), gid)) {
            throw SysError("fchmod");
        }
    }
    if (chmod(fn_.c_str(), sock_file_mode)) {
        throw SysError("fchmod");
    }

    // Listen.
    if (listen(sock_, max_backlog)) {
        throw SysError("listen");
    }
    defer.defuse();
}

SimSocket::SimSocket(SimSocket&& rhs) noexcept
    : sock_(rhs.sock_), suid_(rhs.suid_), fn_(rhs.fn_)
{
    rhs.sock_ = -1;
}

void SimSocket::close()
{
    if (sock_ != -1) {
        ::close(sock_);
        sock_ = -1;
    }
}

SimSocket::~SimSocket()
{
    close();
    // NOTE: if this destructor is called while handling an exception,
    // and PushEUID throws, then std::terminate is called. I'm fine
    // with that because of what PushEUID does.
    PushEUID _(suid_);
    if (unlink(fn_.c_str())) {
        std::clog << "sim: Failed to delete socket <" << fn_ << ">: " << strerror(errno)
                  << std::endl;
    }
}

FD SimSocket::accept()
{
    struct sockaddr_storage sa {
    };
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
    void set_justification(std::string j);

    // Only returns if check approves action. Otherwise loops forever or
    // throws.
    void check();

    [[nodiscard]] static Checker
    make_command(const std::string& socks_dir,
                 uid_t suid,
                 std::string approver,
                 const std::vector<std::string>& args,
                 const std::map<std::string, std::string>& env);

    [[nodiscard]] static Checker make_edit(const std::string& socks_dir,
                                           uid_t suid,
                                           std::string approver,
                                           std::string filename);


private:
    Checker(const std::string& socks_dir,
            uid_t suid,
            std::string approver,
            simproto::ApproveRequest req);

    simproto::ApproveRequest req_;
    const std::string fn_;
    const std::string approver_group_;
    const gid_t approver_gid_;
    SimSocket sock_;
    std::string justification_;
};


// Shared constructor.
Checker::Checker(const std::string& socks_dir,
                 uid_t suid,
                 std::string approver,
                 simproto::ApproveRequest req)
    : req_(std::move(req)),
      approver_group_(std::move(approver)),
      approver_gid_(group_to_gid(approver_group_)),
      fn_(make_sock_filename()),
      sock_(socks_dir + "/" + fn_, suid, approver_gid_)
{
}

Checker Checker::make_command(const std::string& socks_dir,
                              uid_t suid,
                              std::string approver,
                              const std::vector<std::string>& args,
                              const std::map<std::string, std::string>& env)
{
    simproto::ApproveRequest req;

    auto cmd = req.mutable_command();
    {
        std::array<char, PATH_MAX> buf{};
        const char* s = getcwd(buf.data(), buf.size());
        if (s == nullptr) {
            throw SysError("getcwd()");
        }
        cmd->set_cwd(s);
    }
    cmd->set_command(args[0]);
    {
        struct utsname u {
        };
        if (uname(&u)) {
            std::cerr << "sim: failed to get hostname: " << strerror(errno) << "\n";
        } else {
            req.set_host(u.nodename);
        }
    }
    for (const auto& a : args) {
        *cmd->add_args() = a;
    }
    for (const auto& e : env) {
        auto t = cmd->add_environ();
        t->set_key(e.first);
        t->set_value(e.second);
    }
    return Checker(socks_dir, suid, std::move(approver), std::move(req));
}

Checker Checker::make_edit(const std::string& socks_dir,
                           uid_t suid,
                           std::string approver,
                           std::string filename)

{
    simproto::ApproveRequest req;
    auto pb = req.mutable_edit();
    pb->set_filename(std::move(filename));
    return Checker(socks_dir, suid, std::move(approver), std::move(req));
}

void Checker::set_justification(std::string j) { justification_ = std::move(j); }

void Checker::check()
{
    // Construct proto.
    req_.set_id(fn_);
    req_.set_user(uid_to_username(getuid()));

    if (!justification_.empty()) {
        req_.set_justification(justification_);
    }

    // Serialize.
    std::string data;
    if (!req_.SerializeToString(&data)) {
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
        const auto autos = fd.read();
        if (autos.empty()) {
            // Just a probe or async approve request.
            continue;
        }
        if (!resp.ParseFromString(autos)) {
            std::clog << "sim: Failed to parse approval request of size " << autos.size()
                      << "\n";
            continue;
        }
        auto user = uid_to_username(uid);
        if (resp.approved()) {
            std::cerr << "sim: Approved by <" << user << "> (" << uid << ")\n";
            break;
        }
        const auto comment = [&] {
            // TODO: filter to only show safe characters.
            if (resp.has_comment()) {
                return ": " + resp.comment();
            }
            return std::string("");
        }();
        std::cerr << "sim: Rejected by <" << user << "> (" << uid << ")" << comment
                  << "\n";
    }
}

template <typename T>
[[nodiscard]] std::vector<T> repeated_to_vector(std::function<T(int)> get, int count)
{
    std::vector<T> ret(count);
    for (int c = 0; c < ret.size(); c++) {
        ret[c] = get(c);
    }
    return ret;
}

[[nodiscard]] bool is_matched_command(std::function<simproto::CommandDefinition(int)> get,
                                      int count,
                                      const std::vector<std::string>& args)
{
    for (const auto& safe : repeated_to_vector<simproto::CommandDefinition>(
             [&](int index) { return get(index); }, count)) {
        for (const auto& cmd : repeated_to_vector<std::string>(
                 [&](int index) { return safe.command(index); }, safe.command_size())) {
            if (cmd == args[0]) {
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] bool is_safe_command(const simproto::SimConfig& config,
                                   const std::vector<std::string>& args)
{
    return is_matched_command([&](int index) { return config.safe_command(index); },
                              config.safe_command_size(),
                              args);
}

[[nodiscard]] bool is_deny_command(const simproto::SimConfig& config,
                                   const std::vector<std::string>& args)
{
    return is_matched_command([&](int index) { return config.deny_command(index); },
                              config.deny_command_size(),
                              args);
}

[[nodiscard]] std::map<std::string, std::string>
filter_environment(const simproto::SimConfig& config,
                   const std::map<std::string, std::string>& env)
{
    std::map<std::string, std::string> ret;
    for (const auto& ev : env) {
        for (const auto& safe : repeated_to_vector<simproto::EnvironmentDefinition>(
                 [&](int index) { return config.safe_environment(index); },
                 config.safe_environment_size())) {
            const std::regex key_re(safe.key_regex());
            const std::regex value_re(safe.value_regex());

            if (!std::regex_match(ev.first, key_re)) {
                continue;
            }
            if (!std::regex_match(ev.second, value_re)) {
                continue;
            }
            ret[ev.first] = ev.second;
        }
    }
    return ret;
}


[[nodiscard]] std::map<std::string, std::string> environ_map()
{
    std::map<std::string, std::string> ret;
    if (environ == nullptr) {
        return ret;
    }
    for (char** cur = environ; *cur; cur++) {
        const std::string entry(*cur);
        const auto equal_pos = entry.find('=');
        if (equal_pos == std::string::npos) {
            std::clog << "Invalid env data: " << entry << std::endl;
            continue;
        }
        const std::string key(entry.substr(0, equal_pos));
        const std::string value(entry.substr(equal_pos + 1));
        ret[key] = value;
    }
    return ret;
}


[[noreturn]] void usage(const char* av0, int err)
{
    std::cout << av0
              << ": Usage [ -h ] [ -j <justification> ] command... | -e /path/file\n";
    exit(err);
}

void create_sock_dir(const simproto::SimConfig& config, gid_t suid)
{
    PushEUID _(suid);

    struct stat st {
    };
    if (!stat(config.sock_dir().c_str(), &st)) {
        return;
    }
    if (errno != ENOENT) {
        throw SysError("stat(" + config.sock_dir() + ")");
    }
    if (!config.create_sock_dir()) {
        throw std::runtime_error("socket directory " + config.sock_dir() +
                                 " doesn't exist, and create_sock_dir is disabled");
    }

    const gid_t approve_gid = group_to_gid(config.approve_group());

    if (mkdir(config.sock_dir().c_str(), sock_dir_mode)) {
        throw SysError("mkdir(" + config.sock_dir() + ")");
    }
    if (chown(config.sock_dir().c_str(), 0, approve_gid)) {
        throw SysError("chown(" + config.sock_dir() + ")");
    }
}

} // namespace

[[nodiscard]] int mainwrap(int argc, char** argv)
{
    // Save the effective user for later when we re-claim root.
    const uid_t nuid = geteuid();

    if (-1 == seteuid(getuid())) {
        throw SysError("seteuid(getuid)");
    }

    // Option parsing.
    std::string justification;
    int verbose = 0;
    bool edit = false;
    {
        int opt;
        while ((opt = getopt(argc, argv, "+ehj:v")) != -1) {
            switch (opt) {
            case 'e':
                edit = true;
                break;
            case 'h':
                usage(argv[0], EXIT_SUCCESS);
                break;
            case 'j':
                justification = optarg;
                break;
            case 'v':
                verbose++;
                break;
            default: /* '?' */
                usage(argv[0], EXIT_FAILURE);
            }
        }
    }

    if (verbose <= 0) {
        static ::google::protobuf::LogSilencer silence;
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

    if (optind == argc) {
        usage(argv[0], EXIT_FAILURE);
    }

    struct sigaction sigact {
    };
    sigact.sa_handler = sighandler;

    const auto args = args_to_vector(argc - optind, &argv[optind]);
    if (is_deny_command(config, args)) {
        std::cerr << "sim: That command is blocked\n";
        return EXIT_FAILURE;
    }

    const auto envs = filter_environment(config, environ_map());
    if (!is_safe_command(config, args)) {
        // If the sock dir doesn't exist, create it.
        create_sock_dir(config, nuid);
        if (sigaction(SIGINT, &sigact, nullptr)) {
            throw SysError("sigaction");
        }
        std::cerr << "sim: Waiting for MPA approval...\n";
        Checker check = [&] {
            if (edit) {
                return Checker::make_edit(
                    config.sock_dir(), nuid, config.approve_group(), args[0]);
            }
            return Checker::make_command(
                config.sock_dir(), nuid, config.approve_group(), args, envs);
        }();
        if (!justification.empty()) {
            check.set_justification(justification);
        }
        check.check();
    }

    const gid_t ngid = get_primary_group(nuid);

    if (edit) {
        return do_edit(nuid, ngid, args[0]);
    }

    // std::cerr << "sim: command approved!\n";

    // Become fully root.
    // std::clog << "sim: Setting UID " << nuid << std::endl;
    if (setresuid(nuid, nuid, nuid)) {
        throw SysError("setresuid(" + std::to_string(nuid) + ")");
    }
    // std::clog << "sim: Setting GID " << ngid << std::endl;
    if (setresgid(ngid, ngid, ngid)) {
        throw SysError("setresgid(" + std::to_string(ngid) + ")");
    }

    // This only works for root.
    if (setgroups(0, nullptr)) {
        // throw SysError("setgroups(0, nullptr)");
        std::clog << "sim: setgroups(0, nullptr) failed: " << strerror(errno)
                  << std::endl;
    }

    // Clear environment.
    if (clearenv()) {
        throw SysError("clearenv()");
    }
    for (const auto& env : envs) {
        setenv(env.first.c_str(), env.second.c_str(), 1);
    }

    // Execute command.
    execvp(argv[optind], &argv[optind]);
    throw SysError("execvp()");
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
