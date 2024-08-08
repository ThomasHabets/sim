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
use std::os::unix::fs::PermissionsExt;

#[allow(renamed_and_removed_lints)]
mod protos {
    include!(concat!(env!("OUT_DIR"), "/protos/mod.rs"));
}
use protobuf::Message;
use protos::simproto;
use protos::simproto::{ApproveRequest, SimConfig};
use std::io::Read;
use std::io::Write;

#[derive(Parser, Debug)]
struct Opts {
    command: String,
    args: Vec<String>,
}

fn generate_random_filename(length: usize) -> String {
    use rand::Rng;
    let mut rng = rand::thread_rng();
    let filename: String = (&mut rng)
        .sample_iter(&rand::distributions::Alphanumeric)
        .take(length)
        .map(char::from)
        .collect();
    filename
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
fn check_approver(approver_gid: u32, mut sock: std::os::unix::net::UnixStream) -> Result<()> {
    let peer = sock.peer_cred()?;
    eprintln!("DEBUG: Creds: {peer:?}");
    let group = nix::unistd::Group::from_gid(nix::unistd::Gid::from_raw(peer.gid))?
        .ok_or(Error::msg(format!("unknown peer gid {}", peer.gid)))?;
    let peer_user = nix::unistd::User::from_uid(peer.uid.into())?
        .ok_or(Error::msg("peer UID doesn't exist"))?;
    let groups = nix::unistd::getgrouplist(&CString::new(peer_user.name)?, group.gid)?;
    eprintln!("Peer groups: {groups:?}");
    if !groups.into_iter().any(|g| g.as_raw() == approver_gid) {
        return Err(Error::msg("approver is not really an approver"));
    }
    // TODO: properly fill in the approverequest proto.
    let mut req = simproto::ApproveRequest {
        command: protobuf::MessageField::some(simproto::Command {
            cwd: Some("/bleh".to_string()),
            command: Some("somecommand".to_string()),
            ..Default::default()
        }),
        ..Default::default()
    };
    {
        let mut buf = Vec::new();
        req.write_to_vec(&mut buf)?;
        let written = sock.write(&buf)?;
        if written != buf.len() {
            return Err(Error::msg(format!(
                "short write to approver: {written} > {}",
                buf.len()
            )));
        }
    }
    let resp = {
        let mut buf = Vec::new();
        sock.take(1024).read_to_end(&mut buf)?;
        simproto::ApproveResponse::parse_from_bytes(&buf)?
    };
    match resp.approved {
        None => Err(Error::msg("null response")),
        Some(yesno) => {
            if yesno {
                Ok(())
            } else {
                Err(Error::msg("rejected"))
            }
        }
    }
}
struct PushEuid {
    old: nix::unistd::Uid,
}
impl PushEuid {
    fn new(uid: nix::unistd::Uid) -> Result<Self> {
        eprintln!("Setting euid to {uid}");
        let old = nix::unistd::geteuid();
        nix::unistd::seteuid(uid)?;
        Ok(Self { old })
    }
}
impl Drop for PushEuid {
    fn drop(&mut self) {
        eprintln!("Setting euid to {}", self.old);
        nix::unistd::seteuid(self.old).expect("failed to seteuid back to old");
    }
}

fn with_euid<F>(uid: nix::unistd::Uid, f: F) -> Result<()>
where
    F: FnOnce() -> Result<()>,
{
    let _raii = PushEuid::new(uid);
    f()
}

fn get_confirmation(config: &SimConfig, sockname: &str, root: nix::unistd::Uid) -> Result<()> {
    use std::os::fd::FromRawFd;
    use std::os::fd::IntoRawFd;
    let approver_gid = group_to_gid(
        &config
            .approve_group
            .clone()
            .ok_or(Error::msg("approver group doesn't exist"))?,
    )?;
    let listener = socket2::Socket::new(socket2::Domain::UNIX, socket2::Type::SEQPACKET, None)?;
    let sa = socket2::SockAddr::unix(&sockname)?;
    with_euid(root, || {
        listener
            .bind(&sa)
            .map_err(|e| Error::msg(format!("failed to bind to {sockname}: {e}")))?;
        std::os::unix::fs::chown(&sockname, None, Some(approver_gid))?;
        std::fs::set_permissions(sockname, std::fs::Permissions::from_mode(0o660))?;
        Ok(())
    })?;
    listener.listen(5)?;
    loop {
        let (sock, _addr) = listener.accept()?;
        let stream = unsafe { std::os::unix::net::UnixStream::from_raw_fd(sock.into_raw_fd()) };
        match check_approver(approver_gid, stream) {
            Ok(_) => return Ok(()),
            Err(e) => {
                eprintln!("Approver check failed: {e}");
            }
        }
    }
}

fn main() -> Result<()> {
    let saved_euid = nix::unistd::geteuid();
    nix::unistd::seteuid(nix::unistd::Uid::current())?;
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
        + "/"
        + &generate_random_filename(16);
    // TODO: temporarily remove the file if it exists.
    if std::fs::metadata(&sockname).is_ok() {
        std::fs::remove_file(&sockname)?;
    }
    get_confirmation(&config, &sockname, saved_euid)?;
    // become root.
    nix::unistd::setresuid(0.into(), 0.into(), 0.into()).expect("setresuid(0,0,0)");
    nix::unistd::setresgid(0.into(), 0.into(), 0.into()).expect("setresgid(0,0,0)");
    nix::unistd::setgroups(&[]).expect("setgroups([])");
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
