//! ## Creating your bot
//! 1. Open telegram
//! 1. Find BotFather
//! 1. Send `/newbot` to it
//! 1. Name your bot.
//! 1. Save the API token into a file like `env.sh`, with `export TELOXIDE_TOKEN=xxx`.
//!
//! ## Allow the bot to talk to you
//! 1. Go to https://t.me/thenameofyourbothere, and let it open in Telegram.
//! 1. Click "start"
//!
//! ## Find your user ID
//! 1. Find `userinfobot` and start a chat.
//!
//! ## Running the bot
//! 1. Make sure TELOXIDE_TOKEN env is set.
//! 1. Run with `-user_id=1234`
//!
//!
//! TODO:
//! * Garbage collect old polls.
//! * Plug in to PAM, too, to be able to gate SSH logins.
use anyhow::Result;
use clap::Parser;
use log::{debug, info};
use teloxide::prelude::*;
use teloxide::types::{ChatId, InputPollOption, MediaKind, MessageId, MessageKind, PollId};

#[derive(Parser)]
#[command(version)]
struct Opt {
    #[arg(long)]
    user_id: i64,

    #[arg(long, short, default_value_t=2)]
    verbose: usize,
}

async fn wait_for_answer(
    bot: &teloxide::Bot,
    chat_id: ChatId,
    poll_message_id: MessageId,
    poll_id: PollId,
) -> Result<()> {
    let mut offset = 0;
    loop {
        debug!("Waiting for answer…");
        let updates = bot.get_updates().offset(offset).send().await?;
        for update in updates.into_iter() {
            debug!("Update: {update:?}");
            offset = offset.max((update.id.0 + 1).try_into().unwrap());
            if let teloxide::types::UpdateKind::Poll(p) = update.kind {
                if p.id != poll_id {
                    continue;
                }
                let mut approved: Option<bool> = None;
                for option in p.options.iter() {
                    debug!("Option: {:?}", option);
                    if option.voter_count > 0 {
                        if approved.is_some() {
                            panic!("More than one vote!");
                        }
                        approved = Some(match option.text.as_str() {
                            "Approve" => true,
                            "Deny" => false,
                            other => panic!("Unknown option {other}"),
                        });
                    }
                }
                if let Some(a) = approved {
                    println!("Approved: {a:?}");
                    if !p.is_closed {
                        info!("Closing and deleting poll {poll_message_id}");
                        bot.stop_poll(chat_id, poll_message_id).send().await?;
                        bot.delete_message(chat_id, poll_message_id).send().await?;
                        return Ok(());
                    }
                } else {
                    debug!("No answer yet");
                }
            }
        }
        tokio::time::sleep(tokio::time::Duration::from_secs(1)).await;
    }
}

#[tokio::main]
async fn main() -> Result<()> {
    let opt = Opt::parse();
    stderrlog::new()
        .module(module_path!())
        .quiet(false)
        .verbosity(opt.verbose)
        .timestamp(stderrlog::Timestamp::Second)
        .init()?;

    let bot = teloxide::Bot::from_env();
    let chat_id = teloxide::types::ChatId(opt.user_id);
    if false {
        match bot
            .send_message(chat_id, "Hello from Rust Telegram bot!")
            .send()
            .await
        {
            Ok(_) => println!("Message sent."),
            Err(e) => eprintln!("Error sending message: {:?}", e),
        }
    }
    let (msg_id, poll_id) = {
        let msg = bot
            .send_poll(
                chat_id,
                "Some question",
                vec![
                    InputPollOption::new("Approve"),
                    InputPollOption::new("Deny"),
                ],
            )
            .send()
            .await?;
        let msg_id = msg.id;
        let poll_id = match &msg.kind {
            MessageKind::Common(ms) => match &ms.media_kind {
                MediaKind::Poll(p) => p.poll.id.clone(),
                other => panic!("{other:?}"),
            },
            other => panic!("{other:?}"),
        };
        debug!("Sent poll: {msg:?} {msg_id:?}");
        info!("Sent poll: {msg_id:?} {poll_id:?}");
        (msg_id, poll_id)
    };
    wait_for_answer(&bot, chat_id, msg_id, poll_id).await?;
    Ok(())
}
