// -*- c++ -*-
#include <functional>
#include <stdexcept>
#include <string>

namespace Sim {

class Defer
{
public:
    Defer(std::function<void()> func) : func_(func) {}

    // No copy or move.
    Defer(const Defer&) = delete;
    Defer(Defer&&) = delete;
    Defer& operator=(const Defer&) = delete;
    Defer& operator=(Defer&) = delete;

    ~Defer()
    {
        if (active_) {
            func_();
        }
    }
    void defuse() noexcept { active_ = false; }

private:
    bool active_ = true;
    std::function<void()> func_;
};

class SysError : public std::runtime_error
{
public:
    SysError(const std::string& s);
    SysError(const std::string& s, int e);
};

class FD
{
public:
    explicit FD(int fd);

    // No copy.
    FD(const FD&) = delete;
    FD& operator=(const FD&) = delete;

    // Move is fine, if done manually.
    FD(FD&&);
    FD& operator=(FD&&) = delete; // Just temporarily not implemented.

    ~FD();
    void write(const std::string& s);
    void close();
    std::string read();
    uid_t get_uid() const;
    gid_t get_gid() const;

private:
    int fd_;
};


constexpr const char* config_file = "/etc/sim.conf";
std::string uid_to_username(uid_t uid);

} // namespace Sim
