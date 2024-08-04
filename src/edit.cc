/*
 *    Copyright 2020-2023 Google LLC
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
/*
 * This file deals with `sim -e`, where a user asks to edit a file, not run a command.
 *
 * It's pretty tricky.
 *
 * You can't just open the file in an editor as root, because the user
 * can probably start a shell from their editor, or at least open
 * other files.
 *
 * You have to copy the file to a temporary location, and then copy it
 * back when the user exits the editor.
 *
 * But you probably don't want the file to be in the same
 * directory. E.g. non-root owned files in /etc, even temporarily?
 * Eww.
 *
 * You can't rename the file back into the original location, because
 * renames only work within the same filesystem. And there's no
 * guarantee that the original file and /tmp are the same filesystem.
 *
 * And you can't just copy back into the original file. What if you
 * get a write error? That'd corrup the file. No, no good.
 *
 * So what this code does is:
 * 1. Copy the original file to /tmp, as a file owned by the calling user.
 * 2. Open an editor as the user.
 * 3. Copy the file back, into the original file's directory, but a
 *    temporary file called a "rename file".
 * 4. Rename (which is atomic) the "rename file" into the original file,
 *    replacing it.
 *
 * The code is tricky, in order to avoid TOCTOU bugs.
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
#include <memory>
#include <sstream>
#include <vector>
#include <utility>

// POSIX
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Sim {

namespace {
constexpr int temp_filename_len = 32;

// File descriptor wrapper.
class FD
{
public:
    explicit FD(int fd, std::string str) : fd_(fd), str_(std::move(str)) {}

    // Move.
    FD(FD&& rhs) : fd_(std::exchange(rhs.fd_, -1)), str_(std::exchange(rhs.str_, "")) {}

    // No copy.
    FD(const FD& rhs) = delete;
    FD& operator=(const FD& rhs) = delete;

    ~FD() { close(); }
    [[nodiscard]] int fd() const { return fd_; }
    [[nodiscard]] bool bad() const { return fd_ == -1; }
    [[nodiscard]] const std::string& str() const { return str_; }
    void close()
    {
        if (fd_ != -1 && ::close(fd_) == -1) {
            throw std::runtime_error("closing " + str_ + ": " + strerror(errno));
        }
        fd_ = -1;
    }

private:
    int fd_;
    std::string str_;
};

// O_PATH directory wrapper, for use with *at() syscalls.
class Dir
{
public:
    explicit Dir(int fd, const std::string& str) : fd_(fd), str_(str) {}
    ~Dir()
    {
        ::close(fd_);
        fd_ = -1;
    }

    // Move.
    Dir(Dir&& rhs) : fd_(std::exchange(rhs.fd_, -1)), str_(std::exchange(rhs.str_, "")) {}

    Dir& operator=(Dir&& rhs)
    {
        fd_ = std::exchange(rhs.fd_, -1);
        str_ = std::exchange(rhs.str_, "");
        return *this;
    }

    // No copy.
    Dir(const Dir& rhs) = delete;
    Dir& operator=(const Dir& rhs) = delete;

    // Access data.
    [[nodiscard]] int fd() const { return fd_; }
    [[nodiscard]] bool bad() const { return fd_ == -1; }
    [[nodiscard]] const std::string& str() const { return str_; }

    // Open a file in this directory, or throw.
    [[nodiscard]] FD must_open_read(const std::string& fn) const
    {
        if (fd_ != AT_FDCWD && fn.find('/') != std::string::npos) {
            throw std::runtime_error("openat(" + str_ + ", " + fn +
                                     "): can't have slashes in filename");
        }
        FD fd{ openat(fd_, fn.c_str(), O_RDONLY | O_NOFOLLOW), str_ + "/" + fn };
        if (fd.bad()) {
            throw SysError("openat(" + str_ + ", " + fn + ", O_RDONLY | NOFOLLOW)");
        }
        return fd;
    }
    [[nodiscard]] FD must_open_write(const std::string& fn) const
    {
        if (fd_ != AT_FDCWD && fn.find('/') != std::string::npos) {
            throw std::runtime_error("openat(" + str_ + ", " + fn +
                                     "): can't have slashes in filename");
        }
        FD fd{ openat(fd_, fn.c_str(), O_WRONLY | O_NOFOLLOW), str_ + "/" + fn };
        if (fd.bad()) {
            throw SysError("openat(" + str_ + "," + fn + ", O_RDONLY | NOFOLLOW)");
        }
        return fd;
    }
    [[nodiscard]] FD open_create(const std::string& fn) const
    {
        if (fd_ != AT_FDCWD && fn.find('/') != std::string::npos) {
            throw std::runtime_error("openat(" + str_ + ", " + fn +
                                     "): can't have slashes in filename");
        }
        return FD(openat(fd_, fn.c_str(), O_WRONLY | O_NOFOLLOW | O_CREAT | O_EXCL, 0600),
                  str_ + "/" + fn);
    }

private:
    int fd_;
    std::string str_;
};

[[nodiscard]] std::string get_editor()
{
    for (const auto& k : std::vector<const char*>{ "VISUAL", "EDITOR" }) {
        const auto v = getenv(k);
        if (v) {
            return v;
        }
    }
    throw std::runtime_error("no editor selected");
}

[[nodiscard]] std::string get_tmpdir()
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
[[nodiscard]] int editor_main(const std::string& editor, const std::string& fn)
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

// Helper function for tmpfile(), turning the C API mkstemp() into
// what we want in nice C++.
[[nodiscard]] std::string tmpfile_backend(const std::string& s)
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

// Create a temp file that the editor will open.
[[nodiscard]] std::string tmpfile()
{
    return tmpfile_backend(get_tmpdir() + "/sim.XXXXXX");
}

// In order to atomically replace the original file, we need to create
// a file in that same directory. We call this file the "rename file".
[[nodiscard]] std::string renametempfile(const Dir& dir, uid_t uid, const std::string& fn)
{
    PushEUID _(uid);
    for (;;) {
        const std::string name = "sim." + make_random_filename(temp_filename_len);
        FD fd = dir.open_create(name);
        if (!fd.bad()) {
            return name;
        }
    }
}

// Split a path into all its components.
[[nodiscard]] std::pair<std::vector<std::string>, std::string>
split(const std::string& fn)
{
    std::vector<std::string> components;
    std::string cur;
    for (auto ch : fn) {
        if (ch == '/') {
            if (!cur.empty()) {
                components.push_back(cur);
                cur = "";
            }
        } else {
            cur.push_back(ch);
        }
    }
    return { components, cur };
}

// Open an absolute path directory by walking the filesystem from the
// root, without following symlinks.
[[nodiscard]] Dir open_dir(const std::string& fn)
{
    const auto components = split(fn).first;

    Dir dir{ open("/", O_PATH | O_NOFOLLOW | O_DIRECTORY), "/" };
    if (dir.bad()) {
        throw SysError("open(/, O_PATH | O_NOFOLLOW | O_DIRECTORY)");
    }
    for (auto& comp : components) {
        Dir t{ openat(dir.fd(), comp.c_str(), O_PATH | O_NOFOLLOW | O_DIRECTORY),
               dir.str() + "/" + comp };
        if (t.bad()) {
            throw SysError("openat(" + comp + ", O_PATH | O_NOFOLLOW | O_DIRECTORY)");
        }
        dir = std::move(t);
    }
    return dir;
}

// Copy a file by reading a writing.
//
// Optionally set UID to `uid` (root) for opening either file. If the
// `_priv` bools are set to false, the mere normal mortal user will be
// used.
//
// I.e.:
// * orig->tempfile will have (src_priv,dst_priv) be (true, false)
//   It needs to be able to read the source file.
// * tempfile->renamefile will have (false, true)
//   It needs to be able to write into the destination directory.
void copy_file(const uid_t uid,
               const bool src_priv,
               const bool dst_priv,
               const Dir& sdir,
               const std::string& sfn,
               const Dir& ddir,
               const std::string& dfn)
{
    FD src = [&sfn, &sdir, uid, src_priv] {
        std::unique_ptr<PushEUID> priv;
        if (src_priv) {
            priv = std::make_unique<PushEUID>(uid);
        }
        return sdir.must_open_read(sfn);
    }();
    FD dst = [&dfn, &ddir, uid, dst_priv] {
        std::unique_ptr<PushEUID> priv;
        if (dst_priv) {
            priv = std::make_unique<PushEUID>(uid);
        }
        return ddir.must_open_write(dfn);
    }();

    for (;;) {
        char buf[4096];
        const ssize_t rc = read(src.fd(), buf, sizeof(buf));
        if (rc == -1) {
            throw std::runtime_error("read error from " + src.str() + ": " +
                                     strerror(errno));
        }
        if (rc == 0) {
            break;
        }
        const char* p = buf;
        size_t w = rc;
        while (w > 0) {
            const size_t wrc = write(dst.fd(), buf, w);
            if (wrc == -1) {
                throw std::runtime_error("write error to " + dst.str() + ": " +
                                         strerror(errno));
            }
            w -= wrc;
            p += wrc;
        }
    }
}

// Do stat() as uid.
struct stat xstat(const Dir& dir, uid_t uid, const std::string& fn)
{
    PushEUID _(uid);
    struct stat st {
    };
    if (fstatat(dir.fd(), fn.c_str(), &st, 0)) {
        throw SysError("fstatat(" + dir.str() + "," + fn + ")");
    }
    return st;
}

// Return true if the original file's stat() changed between the user
// opening the editor, and us trying to save it.
[[nodiscard]] bool diff_stat(const struct stat& a, const struct stat& b)
{
#define DIFF_FIELD(f) a.st_##f != b.st_##f
    return DIFF_FIELD(dev) || DIFF_FIELD(ino) || DIFF_FIELD(mode) || DIFF_FIELD(uid) ||
           DIFF_FIELD(gid) || DIFF_FIELD(rdev) || DIFF_FIELD(size) || DIFF_FIELD(mtime) ||
           DIFF_FIELD(ctime);
}

// Convert number to octal.
[[nodiscard]] std::string to_oct(const int n)
{
    std::stringstream ss;
    ss << "0" << std::oct << n;
    return ss.str();
}

} // namespace

// Edit the file.
[[nodiscard]] int do_edit(uid_t uid, gid_t gid, const std::string& fn)
{
    const Dir dir = open_dir(fn);
    const std::string base = split(fn).second;
    const Dir cwd{ AT_FDCWD, "." };
    const struct stat orig_st = xstat(dir, uid, base);

    const auto tmpfn = tmpfile();
    Defer _([&tmpfn, &cwd] {
        if (unlinkat(cwd.fd(), tmpfn.c_str(), 0)) {
            std::cerr << "Failed to unlink " << tmpfn << "\n";
        }
    });
    copy_file(uid, true, false, dir, base, cwd, tmpfn);
    spawn_editor(tmpfn);

    const auto renamefn = renametempfile(dir, uid, fn);
    Defer d2([&renamefn, &dir, uid] {
        PushEUID _(uid);
        if (unlinkat(dir.fd(), renamefn.c_str(), 0)) {
            std::cerr << "Failed to unlink " << renamefn << "\n";
        }
    });
    copy_file(uid, false, true, cwd, tmpfn, dir, renamefn);

    {
        PushEUID _(uid);
        const auto mode = orig_st.st_mode & 07777;
        if (fchmodat(dir.fd(), renamefn.c_str(), mode, 0)) {
            throw SysError("fchmodat(" + dir.str() + ", " + renamefn + ", " +
                           to_oct(mode) + ")");
        }
        if (fchownat(dir.fd(), renamefn.c_str(), orig_st.st_uid, orig_st.st_gid, 0)) {
            throw SysError("fchownat(" + dir.str() + ", " + renamefn + ", " +
                           std::to_string(orig_st.st_uid) + ", " +
                           std::to_string(orig_st.st_gid) + ")");
        }

        // Check if original file changed.
        const struct stat new_st = xstat(dir, uid, fn);
        if (diff_stat(orig_st, new_st)) {
            // TODO: ask what to do.
            throw std::runtime_error("race editing file. Try again");
        }

        if (renameat(dir.fd(), renamefn.c_str(), dir.fd(), base.c_str())) {
            throw SysError("renameat(" + dir.str() + "," + renamefn + ", " + fn + ")");
        }
    }
    d2.defuse();
    return EXIT_SUCCESS;
}

} // namespace Sim
