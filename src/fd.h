// -*- c++ -*-
#include <string>

namespace Sim {
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
} // namespace Sim
