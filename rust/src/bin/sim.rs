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
use anyhow::Error;
use anyhow::Result;
use clap::Parser;
use log::debug;
use std::ffi::CString;
use std::os::unix::fs::PermissionsExt;

const MAX_CONFIG_SIZE: u64 = 10485760; // 10 MiB.
const MAX_REPLY_SIZE: u64 = 40960;

#[allow(renamed_and_removed_lints)]
mod protos {
    include!(concat!(env!("OUT_DIR"), "/protos/mod.rs"));
}
use protobuf::Message;
use protos::simproto;
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
    debug!("Admin GID: {admin_gid} {is_admin}");
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

fn make_approve_request(
    opts: &Opts,
    sockname: &std::path::Path,
) -> Result<simproto::ApproveRequest> {
    let mut args = opts.args.clone();
    args.insert(0, opts.command.clone());
    Ok(simproto::ApproveRequest {
        command: protobuf::MessageField::some(simproto::Command {
            cwd: Some(
                std::env::current_dir()?
                    .to_str()
                    .ok_or(Error::msg("cwd couldn't be converted to string"))?
                    .to_string(),
            ),
            command: Some(opts.command.clone()),
            args: args,
            environ: vec![],      // TODO: populate environment.
            ..Default::default()  // Needed because special fields.
        }),
        id: Some(
            sockname
                .file_name()
                .ok_or(Error::msg("sockname doesn't have a file name"))?
                .to_str()
                .ok_or(Error::msg("sockname filename is an invalid string"))?
                .to_string(),
        ),
        host: Some(nix::unistd::gethostname()?.into_string().map_err(|e| {
            Error::msg(format!(
                "hostname has invalid characters or something: {e:?}"
            ))
        })?),
        user: Some(
            nix::unistd::User::from_uid(nix::unistd::Uid::current())?
                .ok_or(Error::msg("unknown current username"))?
                .name,
        ),
        justification: None,
        edit: protobuf::MessageField(None),
        ..Default::default() // Needed because special fields.
    })
}

// This wrapper is only needed because the std implementation of peer_cred is
// nightly only. https://github.com/rust-lang/rust/issues/42839
#[repr(C)]
#[derive(Debug, Default)]
// WARNING: If this struct changes, it must also change identically in peer_cred.c.
struct UCred {
    pid: libc::pid_t,
    uid: libc::uid_t,
    gid: libc::gid_t,
}
extern "C" {
    fn peer_cred_c(sockfd: std::os::fd::RawFd, cred: *mut UCred) -> libc::c_int;
}
fn peer_cred(sock: &std::os::unix::net::UnixStream) -> Result<UCred> {
    let mut cred = UCred::default();
    use std::os::fd::AsRawFd;
    let rc = unsafe { peer_cred_c(sock.as_raw_fd(), &mut cred) };
    if rc != 0 {
        Err(Error::msg(format!(
            "getting peer credentials failed: {}",
            std::io::Error::from_raw_os_error(rc)
        )))
    } else {
        Ok(cred)
    }
}

fn check_approver(
    approver_gid: u32,
    mut sock: std::os::unix::net::UnixStream,
    req: &simproto::ApproveRequest,
) -> Result<()> {
    let peer = peer_cred(&sock)?;
    debug!("sim: Peer creds: {peer:?}");
    let group = nix::unistd::Group::from_gid(nix::unistd::Gid::from_raw(peer.gid))?
        .ok_or(Error::msg(format!("unknown peer gid {}", peer.gid)))?;
    let peer_user = nix::unistd::User::from_uid(peer.uid.into())?
        .ok_or(Error::msg("peer UID doesn't exist"))?;
    let groups = nix::unistd::getgrouplist(&CString::new(peer_user.name)?, group.gid)?;
    debug!("sim: Peer groups: {groups:?}");
    if !groups.into_iter().any(|g| g.as_raw() == approver_gid) {
        return Err(Error::msg("approver is not really an approver"));
    }

    // Write request.
    {
        // With SEQPACKET we need it all in one write.
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

    // Read reply.
    let resp = {
        let mut buf = Vec::new();
        sock.take(MAX_REPLY_SIZE).read_to_end(&mut buf)?;
        simproto::ApproveResponse::parse_from_bytes(&buf)?
    };
    if let Some(comment) = resp.comment {
        return Err(Error::msg(comment));
    }
    match resp.approved {
        None => Err(Error::msg("null response")),
        Some(yesno) => {
            if yesno {
                eprintln!("sim: approved");
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
        debug!("Setting euid to {uid}");
        let old = nix::unistd::geteuid();
        nix::unistd::seteuid(uid)?;
        Ok(Self { old })
    }
}
impl Drop for PushEuid {
    fn drop(&mut self) {
        debug!("Setting euid to {}", self.old);
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

fn get_confirmation(
    opts: &Opts,
    config: &simproto::SimConfig,
    sockname: &std::path::PathBuf,
    root: nix::unistd::Uid,
) -> Result<()> {
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
            .map_err(|e| Error::msg(format!("failed to bind to {}: {e}", sockname.display())))?;
        std::os::unix::fs::chown(&sockname, None, Some(approver_gid))?;
        std::fs::set_permissions(sockname, std::fs::Permissions::from_mode(0o660))?;
        Ok(())
    })?;
    listener.listen(5)?;
    eprintln!("sim: Waiting for MPA approval…");
    let req = make_approve_request(&opts, &sockname)?;
    loop {
        let (sock, _addr) = listener.accept()?;
        let stream = unsafe { std::os::unix::net::UnixStream::from_raw_fd(sock.into_raw_fd()) };
        match check_approver(approver_gid, stream, &req) {
            Ok(_) => return Ok(()),
            Err(e) => {
                eprintln!("sim: Approver check failed: {e}");
            }
        }
    }
}

fn match_command(def: &simproto::CommandDefinition, cmd: &str, _args: &[&str]) -> bool {
    def.command.iter().any(|c| c == cmd)
}

fn check_safe(config: &simproto::SimConfig, opts: &Opts) -> bool {
    config.safe_command.iter().any(|safe| {
        let args: Vec<&str> = opts.args.iter().map(|s| s.as_str()).collect();
        match_command(&safe, &opts.command, &args)
    })
}

fn check_deny(config: &simproto::SimConfig, opts: &Opts) -> Result<()> {
    if config.deny_command.iter().any(|deny| {
        let args: Vec<&str> = opts.args.iter().map(|s| s.as_str()).collect();
        match_command(&deny, &opts.command, &args)
    }) {
        return Err(Error::msg("command is denied"));
    }
    Ok(())
}

fn main() {
    if let Err(e) = wrapped_main() {
        eprintln!("sim: Error: {e}");
        std::process::exit(1);
    }
}

fn wrapped_main() -> Result<()> {
    let saved_euid = nix::unistd::geteuid();
    nix::unistd::seteuid(nix::unistd::Uid::current())?;
    let opts = Opts::try_parse()?;

    let mut config = protos::simproto::SimConfig::new();
    {
        let mut buf = Vec::new();
        std::fs::File::open("/etc/sim.conf")?
            .take(MAX_CONFIG_SIZE)
            .read_to_end(&mut buf)?;
        protobuf::text_format::merge_from_str(&mut config, std::str::from_utf8(&buf)?)
            .map_err(|e| Error::msg(format!("parsing config: {e}")))?;
    }
    debug!("Config: {config:?}");
    check_admin(
        &config
            .admin_group
            .clone()
            .ok_or(Error::msg("config has no admin group set"))?,
    )?;
    check_deny(&config, &opts)?;
    // TODO: handle `edit` commands.
    // TODO: path join.
    if !check_safe(&config, &opts) {
        let sockname = std::path::Path::new(
            &config
                .sock_dir
                .clone()
                .ok_or(Error::msg("config missing sock_dir"))?,
        )
        .join(&generate_random_filename(16));
        get_confirmation(&opts, &config, &sockname, saved_euid)?;
    }
    // become root.
    nix::unistd::setresuid(0.into(), 0.into(), 0.into()).expect("setresuid(0,0,0)");
    nix::unistd::setresgid(0.into(), 0.into(), 0.into()).expect("setresgid(0,0,0)");
    nix::unistd::setgroups(&[]).expect("setgroups([])");
    unsafe {
        // TODO: Preserve chosen environments.
        nix::env::clearenv()?;
    };
    // create env
    debug!(
        "About to execute {:?} with args {:?}",
        opts.command, opts.args
    );
    let args = std::iter::once(opts.command.clone()).chain(opts.args.into_iter());
    let args: Result<Vec<CString>, std::ffi::NulError> = args.map(|a| CString::new(a)).collect();
    let args = args?;
    nix::unistd::execvpe(&CString::new(opts.command)?, &args, &Vec::<CString>::new())?;
    Ok(())
}
