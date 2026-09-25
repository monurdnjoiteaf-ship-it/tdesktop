# Telegram Auto Voice Reply

Windows helper that logs into a Telegram user account and replies to incoming
audio files as OGG/OPUS voice notes.

Supported inputs include MP3, OGG/OGA, OPUS, WAV, M4A, AAC, and FLAC. Existing
voice notes and outgoing messages are ignored to prevent reply loops.

The user enters their own Telegram API ID, API hash, phone number, login code,
and optional two-step verification password locally. No credentials are built
into the executable.

Runtime data is stored in `%APPDATA%\TelegramAutoVoiceReply`.
