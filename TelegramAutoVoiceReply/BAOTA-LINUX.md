# BaoTa Linux installation

1. Upload and extract `Telegram-Auto-Voice-Reply-Linux-x64.zip` to a private
   directory, for example `/www/server/telegram-auto-voice-reply`.
2. In BaoTa Terminal run:

   ```bash
   cd /www/server/telegram-auto-voice-reply
   chmod +x Telegram-Auto-Voice-Reply-Linux-x64
   ./Telegram-Auto-Voice-Reply-Linux-x64
   ```

3. Enter API ID, API Hash, phone number, Telegram login code, and optional
   two-step verification password. Stop it with `Ctrl+C` after login succeeds.
4. In BaoTa `Supervisor Manager`, add a process with:

   - Working directory: `/www/server/telegram-auto-voice-reply`
   - Start command: `/www/server/telegram-auto-voice-reply/Telegram-Auto-Voice-Reply-Linux-x64`
   - Run user: `root` (or use the same user used during the first login)
   - Auto start: enabled

Runtime credentials and the Telegram session are stored under
`~/.local/share/TelegramAutoVoiceReply`. Protect that directory and never share
`account.session` or `config.json`.
