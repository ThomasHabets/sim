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

// Project
#include "util.h"

// C++
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

// POSIX
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Sim {

namespace {

std::string get_editor()
{
    for (const auto& k : std::vector<const char*>{ "VISUAL", "EDITOR" }) {
        const auto v = getenv(k);
        if (v) {
            return v;
        }
    }
    throw std::runtime_error("no editor selected");
}

std::string get_tmpdir()
{
    // TMPDIR is not passed through suid, but the others should be.
    // We only used this path with dropped privileges, so it should be fine.
    for (const auto& k : std::vector<const char*>{ "TMPDIR", "TEMPDIR", "TMP", "TEMP" }) {
        const auto v = getenv(k);
        if (v) {
            return v;
        }
    }
    return "/tmp";
}

void drop_privs()
{
    {
        const gid_t gid = getgid();
        if (setresgid(gid, gid, gid)) {
            throw SysError("setresgid(" + std::to_string(gid) + ")");
        }
    }
    {
        const uid_t uid = getuid();
        if (setresuid(uid, uid, uid)) {
            throw SysError("setresuid(" + std::to_string(uid) + ")");
        }
    }
}

void run_editor(const std::string& editor, const std::string& fn)
{
    execlp(editor.c_str(), editor.c_str(), fn.c_str(), nullptr);
    throw SysError("execlp(" + editor + ")");
}

// child process main function.
int editor_main(const std::string& editor, const std::string& fn)
{
    try {
        run_editor(editor, fn);
    } catch (const std::exception& e) {
        std::cerr << "Editor failed: " << e.what() << std::endl;
    }
    return EXIT_FAILURE;
}

void spawn_editor(const std::string& fn)
{
    const auto editor = get_editor();

    const pid_t pid = fork();
    switch (pid) {
    case -1:
        throw SysError("failed to fork");
    case 0:
        drop_privs();
        _exit(editor_main(editor, fn));
    default:
        // parent
        ;
    }

    const int status = [pid]() {
        int status;
        const pid_t rc = waitpid(pid, &status, 0);
        if (rc == -1) {
            throw SysError("waitpid()-> -1");
        }
        if (rc == -1) {
            throw std::runtime_error("waitpid()-> 0");
        }
        return status;
    }();

    if (WIFSIGNALED(status)) {
        throw std::runtime_error("editor exited by signal " +
                                 std::to_string(WTERMSIG(status)));
    }
    if (!WIFEXITED(status)) {
        throw std::runtime_error("editor exited abnormally");
    }

    const int code = WEXITSTATUS(status);
    if (code) {
        throw std::runtime_error("editor exited with non-zero status " +
                                 std::to_string(code));
    }
}

std::string tmpfile_backend(const std::string& s)
{
    std::vector<char> tmpl(s.begin(), s.end());
    tmpl.push_back(0);
    int fd = mkstemp(tmpl.data());
    if (-1 == fd) {
        throw SysError("mkstemp()");
    }
    ::close(fd);
    tmpl.pop_back(); // remove the null.
    return std::string(tmpl.begin(), tmpl.end());
}

std::string tmpfile() { return tmpfile_backend(get_tmpdir() + "/sim.XXXXXX"); }

std::string renametempfile(uid_t uid, const std::string& fn)
{
    PushEUID _(uid);
    return tmpfile_backend(fn + ".XXXXXX");
}

void copy_file(const uid_t uid,
               const bool src_priv,
               const bool dst_priv,
               const std::string& sfn,
               const std::string& dfn)
{
    std::ifstream src = [&sfn, uid, src_priv] {
        if (src_priv) {
            PushEUID _(uid);
            return std::ifstream(sfn, std::ios::binary);
        }
        return std::ifstream(sfn, std::ios::binary);
    }();
    std::ofstream dst = [&dfn, uid, dst_priv] {
        if (dst_priv) {
            PushEUID _(uid);
            return std::ofstream(dfn, std::ios::binary);
        }
        return std::ofstream(dfn, std::ios::binary);
    }();

    std::istreambuf_iterator<char> src_a(src);
    std::istreambuf_iterator<char> src_b;
    std::ostreambuf_iterator<char> dst_a(dst);
    std::copy(src_a, src_b, dst_a);

    if (!src.good() || !dst.good()) {
        throw std::runtime_error(std::string("copying file for edit: ") +
                                 strerror(errno));
    }
}

// Do stat() as uid.
struct stat xstat(uid_t uid, const std::string& fn)
{
    PushEUID _(uid);
    struct stat st {
    };
    if (stat(fn.c_str(), &st)) {
        throw SysError("stat");
    }
    return st;
}

bool diff_stat(const struct stat& a, const struct stat& b)
{
#define DIFF_FIELD(f) a.st_##f != b.st_##f
    return DIFF_FIELD(dev) || DIFF_FIELD(ino) || DIFF_FIELD(mode) || DIFF_FIELD(uid) ||
           DIFF_FIELD(gid) || DIFF_FIELD(rdev) || DIFF_FIELD(size) || DIFF_FIELD(mtime) ||
           DIFF_FIELD(ctime);
}

std::string to_oct(const int n)
{
    std::stringstream ss;
    ss << "0" << std::oct << n;
    return ss.str();
}

} // namespace

int do_edit(uid_t uid, gid_t gid, const std::string& fn)
{
    const struct stat orig_st = xstat(uid, fn);

    const auto tmpfn = tmpfile();
    Defer _([&tmpfn] {
        if (unlink(tmpfn.c_str())) {
            std::cerr << "Failed to unlink " << tmpfn << "\n";
        }
    });
    copy_file(uid, true, false, fn, tmpfn);
    spawn_editor(tmpfn);

    const auto renamefn = renametempfile(uid, fn);
    Defer d2([&renamefn, uid] {
        PushEUID _(uid);
        if (unlink(renamefn.c_str())) {
            std::cerr << "Failed to unlink " << renamefn << "\n";
        }
    });
    copy_file(uid, false, true, tmpfn, renamefn);

    {
        PushEUID _(uid);
        const auto mode = orig_st.st_mode & 07777;
        if (chmod(renamefn.c_str(), mode)) {
            throw SysError("chmod(" + renamefn + ", " + to_oct(mode) + ")");
        }
        if (chown(renamefn.c_str(), orig_st.st_uid, orig_st.st_gid)) {
            throw SysError("chown(" + renamefn + ", " + std::to_string(orig_st.st_uid) +
                           ", " + std::to_string(orig_st.st_gid) + ")");
        }

        // Check if changed.
        const struct stat new_st = xstat(uid, fn);
        if (diff_stat(orig_st, new_st)) {
            // TODO: ask what to do.
            throw std::runtime_error("race editing file. TODO: ask what to do");
        }

        if (rename(renamefn.c_str(), fn.c_str())) {
            throw SysError("rename(" + renamefn + ", " + fn + ")");
        }
    }
    d2.defuse();
    return EXIT_SUCCESS;
}

} // namespace Sim
