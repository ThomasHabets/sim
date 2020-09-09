// -*- c++ -*-
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
};

constexpr const char* config_file = "/etc/sim.conf";
std::string uid_to_username(uid_t uid);
gid_t group_to_gid(const std::string& group);
bool user_is_member(const std::string& user, const gid_t gid, const std::string& group);

} // namespace Sim
