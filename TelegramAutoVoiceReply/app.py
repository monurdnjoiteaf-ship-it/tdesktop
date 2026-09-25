import asyncio
import io
import json
import os
import subprocess
import sys
import threading
import traceback
import webbrowser
from collections import deque
from pathlib import Path
import tkinter as tk
from tkinter import messagebox, scrolledtext, simpledialog, ttk

import imageio_ffmpeg
from telethon import TelegramClient, events
from telethon.errors import SessionPasswordNeededError


APP_NAME = "Telegram Auto Voice Reply"
AUDIO_EXTENSIONS = {
    ".aac", ".flac", ".m4a", ".mp3", ".oga", ".ogg", ".opus", ".wav"
}


def data_directory() -> Path:
    base = Path(os.environ.get("APPDATA", Path.home()))
    result = base / "TelegramAutoVoiceReply"
    result.mkdir(parents=True, exist_ok=True)
    return result


DATA_DIR = data_directory()
CONFIG_PATH = DATA_DIR / "config.json"
SESSION_PATH = DATA_DIR / "account"


class AutoVoiceApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("Telegram 音频自动回复助手")
        self.root.geometry("760x560")
        self.root.minsize(680, 500)

        self.api_id = tk.StringVar()
        self.api_hash = tk.StringVar()
        self.phone = tk.StringVar()
        self.remember = tk.BooleanVar(value=True)
        self.status = tk.StringVar(value="未启动")

        self.worker_thread = None
        self.loop = None
        self.client = None
        self.stop_requested = threading.Event()

        self._build_ui()
        self._load_config()
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    def _build_ui(self) -> None:
        outer = ttk.Frame(self.root, padding=14)
        outer.pack(fill="both", expand=True)
        outer.columnconfigure(1, weight=1)
        outer.rowconfigure(6, weight=1)

        ttk.Label(outer, text="API ID").grid(row=0, column=0, sticky="w", pady=5)
        self.api_id_entry = ttk.Entry(outer, textvariable=self.api_id)
        self.api_id_entry.grid(row=0, column=1, sticky="ew", pady=5, padx=(10, 0))

        ttk.Label(outer, text="API Hash").grid(row=1, column=0, sticky="w", pady=5)
        self.api_hash_entry = ttk.Entry(outer, textvariable=self.api_hash, show="*")
        self.api_hash_entry.grid(row=1, column=1, sticky="ew", pady=5, padx=(10, 0))

        ttk.Label(outer, text="手机号").grid(row=2, column=0, sticky="w", pady=5)
        self.phone_entry = ttk.Entry(outer, textvariable=self.phone)
        self.phone_entry.grid(row=2, column=1, sticky="ew", pady=5, padx=(10, 0))

        options = ttk.Frame(outer)
        options.grid(row=3, column=0, columnspan=2, sticky="ew", pady=(6, 4))
        ttk.Checkbutton(
            options,
            text="在本机保存 API 信息",
            variable=self.remember,
        ).pack(side="left")
        ttk.Button(
            options,
            text="申请 API ID / Hash",
            command=lambda: webbrowser.open("https://my.telegram.org/apps"),
        ).pack(side="right")

        controls = ttk.Frame(outer)
        controls.grid(row=4, column=0, columnspan=2, sticky="ew", pady=8)
        self.start_button = ttk.Button(controls, text="登录并启动", command=self.start)
        self.start_button.pack(side="left")
        self.stop_button = ttk.Button(
            controls, text="停止", command=self.stop, state="disabled"
        )
        self.stop_button.pack(side="left", padx=8)
        ttk.Label(controls, textvariable=self.status).pack(side="right")

        note = (
            "运行规则：收到别人发来的普通音频文件后，自动转换为 OGG/OPUS 语音并回复原消息。"
            "自己发出的消息和已经是语音的信息会被忽略，避免循环。"
        )
        ttk.Label(outer, text=note, wraplength=710).grid(
            row=5, column=0, columnspan=2, sticky="w", pady=(0, 8)
        )

        self.log_box = scrolledtext.ScrolledText(outer, height=18, state="disabled")
        self.log_box.grid(row=6, column=0, columnspan=2, sticky="nsew")

    def _load_config(self) -> None:
        try:
            data = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return
        self.api_id.set(str(data.get("api_id", "")))
        self.api_hash.set(str(data.get("api_hash", "")))
        self.phone.set(str(data.get("phone", "")))
        self.remember.set(bool(data.get("remember", True)))

    def _save_config(self) -> None:
        if not self.remember.get():
            try:
                CONFIG_PATH.unlink(missing_ok=True)
            except OSError:
                pass
            return
        data = {
            "api_id": self.api_id.get().strip(),
            "api_hash": self.api_hash.get().strip(),
            "phone": self.phone.get().strip(),
            "remember": True,
        }
        CONFIG_PATH.write_text(
            json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8"
        )

    def log(self, text: str) -> None:
        def append() -> None:
            self.log_box.configure(state="normal")
            self.log_box.insert("end", text.rstrip() + "\n")
            self.log_box.see("end")
            self.log_box.configure(state="disabled")

        self.root.after(0, append)

    def _set_running(self, running: bool, status: str) -> None:
        def apply() -> None:
            self.status.set(status)
            self.start_button.configure(state="disabled" if running else "normal")
            self.stop_button.configure(state="normal" if running else "disabled")
            state = "disabled" if running else "normal"
            self.api_id_entry.configure(state=state)
            self.api_hash_entry.configure(state=state)
            self.phone_entry.configure(state=state)

        self.root.after(0, apply)

    def ask(self, title: str, prompt: str, password: bool = False) -> str | None:
        ready = threading.Event()
        answer = {"value": None}

        def show() -> None:
            answer["value"] = simpledialog.askstring(
                title, prompt, parent=self.root, show="*" if password else None
            )
            ready.set()

        self.root.after(0, show)
        ready.wait()
        return answer["value"]

    def start(self) -> None:
        api_id_text = self.api_id.get().strip()
        api_hash = self.api_hash.get().strip()
        phone = self.phone.get().strip()
        if not api_id_text.isdigit() or not api_hash or not phone:
            messagebox.showerror("信息不完整", "请填写正确的 API ID、API Hash 和手机号。")
            return
        self._save_config()
        self.stop_requested.clear()
        self._set_running(True, "正在连接…")
        self.worker_thread = threading.Thread(
            target=self._thread_main,
            args=(int(api_id_text), api_hash, phone),
            daemon=True,
        )
        self.worker_thread.start()

    def stop(self) -> None:
        self.stop_requested.set()
        self.status.set("正在停止…")
        if self.loop and self.client:
            asyncio.run_coroutine_threadsafe(self.client.disconnect(), self.loop)

    def _thread_main(self, api_id: int, api_hash: str, phone: str) -> None:
        try:
            asyncio.run(self._run_client(api_id, api_hash, phone))
        except Exception as error:
            self.log(f"错误：{error}")
            self.log(traceback.format_exc())
        finally:
            self.client = None
            self.loop = None
            self._set_running(False, "已停止")

    async def _run_client(self, api_id: int, api_hash: str, phone: str) -> None:
        self.loop = asyncio.get_running_loop()
        self.client = TelegramClient(str(SESSION_PATH), api_id, api_hash)
        await self.client.connect()

        if not await self.client.is_user_authorized():
            self.log("正在发送登录验证码…")
            await self.client.send_code_request(phone)
            code = self.ask("Telegram 登录", "请输入 Telegram 收到的登录验证码：")
            if not code:
                self.log("已取消登录。")
                return
            try:
                await self.client.sign_in(phone=phone, code=code.strip())
            except SessionPasswordNeededError:
                password = self.ask(
                    "两步验证", "请输入 Telegram 两步验证密码：", password=True
                )
                if not password:
                    self.log("已取消登录。")
                    return
                await self.client.sign_in(password=password)

        me = await self.client.get_me()
        display = " ".join(filter(None, [me.first_name, me.last_name])) or str(me.id)
        self.log(f"登录成功：{display}")
        self.log("自动回复已经启动，等待音频文件…")
        self._set_running(True, "运行中")

        processed = set()
        processed_order = deque()
        semaphore = asyncio.Semaphore(2)

        @self.client.on(events.NewMessage(incoming=True))
        async def on_message(event) -> None:
            message = event.message
            key = (event.chat_id, message.id)
            if key in processed or not self._is_regular_audio(message):
                return
            processed.add(key)
            processed_order.append(key)
            if len(processed_order) > 2000:
                processed.discard(processed_order.popleft())

            async with semaphore:
                name = getattr(message.file, "name", None) or "audio"
                self.log(f"收到音频：{name}，正在转换…")
                try:
                    source = await self.client.download_media(message, file=bytes)
                    if not source:
                        raise RuntimeError("下载音频失败")
                    voice = await asyncio.to_thread(self._convert_to_voice, source)
                    output = io.BytesIO(voice)
                    output.name = "voice.ogg"
                    await self.client.send_file(
                        event.chat_id,
                        output,
                        voice_note=True,
                        reply_to=message.id,
                    )
                    self.log(f"已回复语音：{name}")
                except Exception as error:
                    self.log(f"处理失败：{name}：{error}")

        await self.client.run_until_disconnected()

    @staticmethod
    def _is_regular_audio(message) -> bool:
        if not message or not message.document or message.voice:
            return False
        file = message.file
        mime = (getattr(file, "mime_type", None) or "").lower()
        name = (getattr(file, "name", None) or "").lower()
        return mime.startswith("audio/") or Path(name).suffix in AUDIO_EXTENSIONS

    @staticmethod
    def _convert_to_voice(source: bytes) -> bytes:
        ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
        command = [
            ffmpeg,
            "-hide_banner",
            "-loglevel",
            "error",
            "-i",
            "pipe:0",
            "-map_metadata",
            "-1",
            "-vn",
            "-ac",
            "1",
            "-ar",
            "48000",
            "-c:a",
            "libopus",
            "-b:a",
            "32k",
            "-application",
            "voip",
            "-f",
            "ogg",
            "pipe:1",
        ]
        flags = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
        result = subprocess.run(
            command,
            input=source,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            creationflags=flags,
            check=False,
        )
        if result.returncode or not result.stdout:
            detail = result.stderr.decode("utf-8", "replace").strip()
            raise RuntimeError(detail or "FFmpeg 转换失败")
        return result.stdout

    def _on_close(self) -> None:
        self.stop()
        self.root.after(300, self.root.destroy)


def main() -> None:
    root = tk.Tk()
    try:
        ttk.Style().theme_use("vista")
    except tk.TclError:
        pass
    AutoVoiceApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
