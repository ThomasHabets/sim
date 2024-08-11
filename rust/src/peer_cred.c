// This wrapper is only needed because the std implementation of peer_cred is
// nightly only. https://github.com/rust-lang/rust/issues/42839
#define _GNU_SOURCE
#include <sys/socket.h>
#include <errno.h>

// WARNING: If this struct changes, it must also change identically in sim.rs.
struct UCred {
    pid_t pid;
    uid_t uid;
    gid_t gid;
};

int peer_cred_c(int sockfd, struct UCred* cred)
{
    socklen_t len = sizeof(struct ucred);
    struct ucred ucred;
    cred->pid = -1;
    cred->uid = -1;
    cred->gid = -1;

    if (getsockopt(sockfd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) == -1) {
        return errno;
    }

    cred->pid = ucred.pid;
    cred->uid = ucred.uid;
    cred->gid = ucred.gid;
    return 0;
}
