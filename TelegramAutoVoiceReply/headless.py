import argparse
import asyncio
import getpass
import io
import json
import os
import subprocess
import sys
from collections import deque
from pathlib import Path

import imageio_ffmpeg
from telethon import TelegramClient, events
from telethon.errors import SessionPasswordNeededError


AUDIO_EXTENSIONS = {
    ".aac", ".flac", ".m4a", ".mp3", ".oga", ".ogg", ".opus", ".wav"
}
DATA_DIR = Path(
    os.environ.get(
        "TELEGRAM_AUTO_VOICE_DATA",
        Path.home() / ".local" / "share" / "TelegramAutoVoiceReply",
    )
)
CONFIG_PATH = DATA_DIR / "config.json"
SESSION_PATH = DATA_DIR / "account"


def log(text: str) -> None:
    print(text, flush=True)


def load_config(force: bool) -> dict:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    config = {}
    if CONFIG_PATH.exists() and not force:
        try:
            config = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            config = {}

    api_id = str(config.get("api_id", "")).strip()
    api_hash = str(config.get("api_hash", "")).strip()
    phone = str(config.get("phone", "")).strip()
    if force or not api_id.isdigit():
        api_id = input("Telegram API ID: ").strip()
    if force or not api_hash:
        api_hash = getpass.getpass("Telegram API Hash: ").strip()
    if force or not phone:
        phone = input("Telegram phone number (for example +8613800000000): ").strip()
    if not api_id.isdigit() or not api_hash or not phone:
        raise RuntimeError("API ID, API Hash, and phone number are required")

    result = {"api_id": int(api_id), "api_hash": api_hash, "phone": phone}
    CONFIG_PATH.write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    try:
        CONFIG_PATH.chmod(0o600)
    except OSError:
        pass
    return result


def is_regular_audio(message) -> bool:
    if not message or not message.document or message.voice:
        return False
    file = message.file
    mime = (getattr(file, "mime_type", None) or "").lower()
    name = (getattr(file, "name", None) or "").lower()
    return mime.startswith("audio/") or Path(name).suffix in AUDIO_EXTENSIONS


def convert_to_voice(source: bytes) -> bytes:
    command = [
        imageio_ffmpeg.get_ffmpeg_exe(),
        "-hide_banner", "-loglevel", "error", "-i", "pipe:0",
        "-map_metadata", "-1", "-vn", "-ac", "1", "-ar", "48000",
        "-c:a", "libopus", "-b:a", "32k", "-application", "voip",
        "-f", "ogg", "pipe:1",
    ]
    result = subprocess.run(
        command,
        input=source,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode or not result.stdout:
        detail = result.stderr.decode("utf-8", "replace").strip()
        raise RuntimeError(detail or "FFmpeg conversion failed")
    return result.stdout


async def run(config: dict) -> None:
    client = TelegramClient(
        str(SESSION_PATH), config["api_id"], config["api_hash"]
    )
    await client.connect()
    if not await client.is_user_authorized():
        await client.send_code_request(config["phone"])
        code = input("Telegram login code: ").strip()
        try:
            await client.sign_in(phone=config["phone"], code=code)
        except SessionPasswordNeededError:
            password = getpass.getpass("Telegram two-step verification password: ")
            await client.sign_in(password=password)

    me = await client.get_me()
    display = " ".join(filter(None, [me.first_name, me.last_name])) or str(me.id)
    log(f"Logged in as {display}. Auto voice reply is running.")

    processed = set()
    processed_order = deque()
    semaphore = asyncio.Semaphore(2)

    @client.on(events.NewMessage(incoming=True))
    async def on_message(event) -> None:
        message = event.message
        key = (event.chat_id, message.id)
        if key in processed or not is_regular_audio(message):
            return
        processed.add(key)
        processed_order.append(key)
        if len(processed_order) > 2000:
            processed.discard(processed_order.popleft())

        async with semaphore:
            name = getattr(message.file, "name", None) or "audio"
            log(f"Received {name}; converting...")
            try:
                source = await client.download_media(message, file=bytes)
                if not source:
                    raise RuntimeError("audio download failed")
                voice = await asyncio.to_thread(convert_to_voice, source)
                output = io.BytesIO(voice)
                output.name = "voice.ogg"
                await client.send_file(
                    event.chat_id,
                    output,
                    voice_note=True,
                    reply_to=message.id,
                )
                log(f"Replied with voice: {name}")
            except Exception as error:
                log(f"Failed to process {name}: {error}")

    await client.run_until_disconnected()


def main() -> None:
    parser = argparse.ArgumentParser(description="Telegram automatic voice reply")
    parser.add_argument(
        "--configure", action="store_true", help="replace saved Telegram settings"
    )
    args = parser.parse_args()
    config = load_config(args.configure)
    asyncio.run(run(config))


if __name__ == "__main__":
    main()
