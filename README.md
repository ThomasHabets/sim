# sim

This is not a Google product.

Sim is something like sudo/doas, but instead of asking for your password every
time you run a command, it requires a second user to approve the command you
run.

https://github.com/ThomasHabets/sim

## Installing

Only needed if building from git repo:
```
./bootstrap.sh
```

Then:

```
./configure && make && make install
chmod 4711 /usr/local/bin/sim
```

## Setting up

Create two groups. `sim`, and `sim-approvers`. The former are admins,
the latter are approver admin commands. A user can be a member of
both, but can't approve their own commands.

### Create config file

```
cat > /etc/sim.conf
sock_dir: "/var/run/sim"
admin_group: "sim"
approve_group: "sim-approvers"
^D
```

### Create socket directory

NOTE: this may have to be done at every boot if this is on a tmpfs!

```
mkdir /var/run/sim
chown root:sim-approvers /var/run/sim
chmod 750 /var/run/sim
```

## Running

### Admin runs this

```
$ sim id
sim: Waiting for MPA approval...
sim: Approved by <some-approver-user> (1001)
bin   cdrom                 dev  home        initrd.img.old  lib32  lost+found  mnt  proc  run   srv  tmp  var      vmlinuz.old
boot  check_permissions.py  etc  initrd.img  lib             lib64  media       opt  root  sbin  sys  usr  vmlinuz
```


### Approver runs this

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
