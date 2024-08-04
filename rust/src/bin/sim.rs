/*
 *    Copyright 2024 Google LLC
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
#![feature(peer_credentials_unix_socket)]

use anyhow::Error;
use anyhow::Result;
use clap::Parser;
use std::ffi::CString;

mod protos {
    include!(concat!(env!("OUT_DIR"), "/protos/mod.rs"));
}
use protos::simproto::SimConfig;

#[derive(Parser, Debug)]
struct Opts {
    command: String,
    args: Vec<String>,
}

fn group_to_gid(name: &str) -> Result<u32> {
    Ok(nix::unistd::Group::from_name(&name)?
        .ok_or(Error::msg(format!("no such group {name}")))?
        .gid
        .as_raw())
}
fn check_admin(admin_group: &str) -> Result<()> {
    let admin_gid = group_to_gid(admin_group)?;
    let is_admin = nix::unistd::getgroups()?
        .into_iter()
        .any(|g| g.as_raw() == admin_gid);
    eprintln!("Admin GID: {admin_gid} {is_admin}");
    if is_admin {
        Ok(())
    } else {
        let uid = nix::unistd::Uid::current();
        let user = nix::unistd::User::from_uid(uid)?.ok_or(Error::msg(format!(
            "current user uid {uid} has no user. Huh?"
        )))?;
        Err(Error::msg(format!(
            "user <{}> is not in admin group",
            user.name
        )))
    }
}
fn check_approver(config: &SimConfig, sock: std::os::unix::net::UnixStream) -> Result<()> {
    let peer = sock.peer_cred()?;
    eprintln!("DEBUG: Creds: {peer:?}");
    //let addr = sock.peer_addr()?;
    // eprintln!("Addr: {addr:?}");
    let group = nix::unistd::Group::from_gid(nix::unistd::Gid::from_raw(peer.gid))?
        .ok_or(Error::msg(format!("unknown pere gid {}", peer.gid)))?;
    let groups = nix::unistd::getgrouplist(
        &CString::new(
            config
                .approve_group
                .clone()
                .ok_or(Error::msg("approver group doesn't exist"))?,
        )?,
        group.gid,
    )?;
    eprintln!("Peer groups: {groups:?}");
    let approver_gid = group_to_gid(
        &config
            .approve_group
            .clone()
            .ok_or(Error::msg("approver group doesn't exist"))?,
    )?;
    if !groups.into_iter().any(|g| g.as_raw() == approver_gid) {
        return Err(Error::msg("approver is not really an approver"));
    }
    Ok(())
}

fn get_confirmation(config: &SimConfig, sockname: &str) -> Result<()> {
    use std::os::fd::FromRawFd;
    use std::os::fd::IntoRawFd;
    let listener = socket2::Socket::new(socket2::Domain::UNIX, socket2::Type::SEQPACKET, None)?;
    listener.bind(&socket2::SockAddr::unix(&sockname)?)?;
    listener.listen(5);
    loop {
        let (sock, addr) = listener.accept()?;
        let mut stream = unsafe { std::os::unix::net::UnixStream::from_raw_fd(sock.into_raw_fd()) };
        check_approver(&config, stream)?;
    }
}

fn main() -> Result<()> {
    let opts = Opts::try_parse()?;

    let mut config = protos::simproto::SimConfig::new();
    protobuf::text_format::merge_from_str(
        &mut config,
        std::str::from_utf8(&std::fs::read("/etc/sim.conf")?)?,
    )?;
    eprintln!("Config: {config:?}");
    if false {
        // TODO: enable.
        check_admin(
            &config
                .admin_group
                .clone()
                .ok_or(Error::msg("config has no admin group set"))?,
        )?;
    }
    // TODO: check deny command.
    // TODO: handle `edit` commands.
    // TODO: filter environments.
    // TODO: actually check if the command should be allowed.
    // TODO: path join.
    let sockname = config
        .sock_dir
        .clone()
        .ok_or(Error::msg("config missing sock_dir"))?
        + "/bleh.sock";
    // TODO: temporarily remove the file if it exists.
    if std::fs::metadata(&sockname).is_ok() {
        std::fs::remove_file(&sockname)?;
    }
    get_confirmation(&config, &sockname)?;
    // become root.
    nix::unistd::setresuid(0.into(), 0.into(), 0.into())?;
    nix::unistd::setresgid(0.into(), 0.into(), 0.into())?;
    nix::unistd::setgroups(&[])?;
    unsafe {
        nix::env::clearenv()?;
    };
    // create env
    println!(
        "About to execute {:?} with args {:?}",
        opts.command, opts.args
    );
    let args = std::iter::once(opts.command.clone()).chain(opts.args.into_iter());
    let args: Result<Vec<CString>, std::ffi::NulError> = args.map(|a| CString::new(a)).collect();
    let args = args?;
    nix::unistd::execvpe(&CString::new(opts.command)?, &args, &Vec::<CString>::new())?;
    Ok(())
}
