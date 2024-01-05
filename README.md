# sim

This is not a Google product.

Sim is something like sudo/doas, but instead of asking for your password every
time you run a command, it requires a second user to approve the command you
run.

https://github.com/ThomasHabets/sim

## Why

sudo/doas is often configured to ask for a password every time you run a
command, or at least ask sometimes. This to me makes no sense.

If someone has access to your normal user shell account, then they can reroute
so that next time you run sudo/doas it'll record your password.

This means that your admin's account is basically a root account with only
mistake-protection, not attacker-protection.

If we've learned anything in the last few years it's that "something you know"
isn't a great authentication factor. We could already use hardware keys to log
in over SSH (e.g. [yubikey][yubikey] or [TPM chips][tpm]), and/or [use one-time
passwords][otp].

OTPs are useful for sudo, but still allows an attacker to man-in-the-middle your
command. I.e. you think you're running `sudo echo foo`, but the attacker who
controls your account already actually makes it run
`sudo bash -c "echo foo; echo […] >> /root/.ssh/authorized_keys"`.

This project aims to close that hole, just like with some banks when you
initiate some transfers or payments you have to open an app on your phone and
approve exactly what that transfer is, not just "do you approve **a** transfer",
but exactly **what** you're approving.

[yubikey]: https://blog.habets.se/2016/01/Yubikey-4-for-SSH-with-physical-presence-proof.html
[tpm]: https://blog.habets.se/2013/11/TPM-chip-protecting-SSH-keys-properly.html
[otp]: https://github.com/google/google-authenticator-libpam/

## Future work

* Approver Web UI
* PAM module for approving logins, perhaps
* Command logging for audit

## Installing

This for now only covers building and setting up the local tools `sim`
and `approve`. Setting up an app as approver will be covered by future
documentation.

### Dependencies

```
apt install libprotobuf-dev protobuf-compiler pkg-config
```

### Building

Only needed if building from git repo:
```
apt install autoconf automake
./bootstrap.sh
```

Then:

```
./configure && make && make install
```

## Setting up

Create two groups. `sim-admins`, and `sim-approvers`. The former are admins,
the latter are approver of admin commands. A user can be a member of
both, but can't approve their own commands.

Members of `sim-approvers` can't run `sim`, only `approve`. Unless they
are also members of `sim-admins`.

### Create config file

```
cat > /etc/sim.conf
sock_dir: "/var/run/sim"
admin_group: "sim-admins"
approve_group: "sim-approvers"
^D
```

The socket directory will be automatically created, but its parent
directly (in this example `/var/run`) must already exist.

## Running

### Admin runs this

```
$ sim id
sim: Waiting for MPA approval...
sim: Approved by <some-approver-user> (1001)
uid=0(root) gid=0(root) groups=0(root)
```

```
$ sim ls /
sim: Waiting for MPA approval...
sim: Approved by <some-approver-user> (1001)
bin   cdrom                 dev  home        initrd.img.old  lib32  lost+found  mnt  proc  run   srv  tmp  var      vmlinuz.old
boot  check_permissions.py  etc  initrd.img  lib             lib64  media       opt  root  sbin  sys  usr  vmlinuz
```

### Approver runs this

```
$ approve
Picking up F2464EC0FA9573101125D17B7D084AD0
From user <some-admin-user> (1000)
------------------
command {
  cwd: "/home/some-admin-user/scm/sim"
  command: "id"
  args: "id"
}
------------------
Approve? [y]es / [n]o / [c]omment> y
```

```
$ approve
Picking up 0298A7B7EEF204D30F643F2AED1FF037
From user <some-admin-user> (1000)
------------------
command {
  cwd: "/home/some-admin-user/scm/sim"
  command: "ls"
  args: "ls"
  args: "/"
}
------------------
Approve? [y]es / [n]o / [c]omment> y
```

## Setup on non-linux

## OpenBSD

```
pkg_add git automake autoconf protobuf protobuf-compiler
git clone https://github.com/ThomasHabets/sim
cd sim
AUTOCONF_VERSION=2.69 AUTOMAKE_VERSION=1.16 ./bootstrap.sh
./configure CPPFLAGS=-I/usr/local/include LDFLAGS=-L/usr/local/lib CXX=clang++
make
make install
```

## FAQ

### Sounds like a lot of waiting for another human

Yes, could be. I aim to improve this by adding an allow list for command
patterns that can run without approval. E.g. most will probably allow `ls` to
run without approval, as the audit trail may be enough.

But you have peer review for code, don't you? Why not peer review for adding
users, removing files, and installing software?

An upcoming feature is also to be able to approve your own commands
out-of-band. E.g. via your phone. No waiting then, and it'll make it more useful
for hobby applications where they may only actually be one human admin.

But also, and especially for large installations, if you see every need to run
an ad-hoc command as a failure, then this makes more sense.

E.g.:

* Why do you need ad-hoc root access in order to install a new TLS cert?
  Shouldn't that be automated?

* Why do you need to run this complex command? Shouldn't that be a peer-reviewed
  script that's checked into source control?
