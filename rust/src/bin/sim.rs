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
use std::ffi::CString;

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

fn main() -> Result<()> {
    let opts = Opts::try_parse()?;
    // TODO: read config.
    let admin_group = "sim-admins";
    if false {
        // TODO: enable.
        check_admin(admin_group)?;
    }
    // check deny command.
    // handle `edit` commands.
    // filter environments.
    // TODO: actually check if the command should be allowed.
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
