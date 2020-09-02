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
    uid_t getUID() const;

private:
    int fd_;
};
