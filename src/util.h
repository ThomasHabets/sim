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

constexpr const char* config_file = "/etc/sim.conf";
std::string uid_to_username(uid_t uid);
gid_t group_to_gid(const std::string& group);
bool user_is_member(const std::string& user, const gid_t gid, const std::string& group);

} // namespace Sim
