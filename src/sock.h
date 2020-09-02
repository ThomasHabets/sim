// -*- c++ -*-
#include <stdexcept>
#include <string>

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
    explicit FD(const FD&) = delete;
    FD(FD&&);
    ~FD();
    void write(const std::string& s);
    std::string read();
    uid_t get_uid() const;
    gid_t get_gid() const;

private:
    int fd_;
};


constexpr const char* config_file = "/etc/sim.conf";
