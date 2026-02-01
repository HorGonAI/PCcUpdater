import logging
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path
from typing import Iterable, Optional, Tuple

import requests
from telegram import Update
from telegram.constants import ChatAction
from telegram.ext import (
    ApplicationBuilder,
    CommandHandler,
    ContextTypes,
    MessageHandler,
    filters,
)

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
)
logger = logging.getLogger("pc_updater_bot")


class ConfigError(RuntimeError):
    pass


def resolve_target_dir() -> Path:
    target_dir = os.getenv("PC_UPDATER_TARGET_DIR")
    if not target_dir:
        raise ConfigError("PC_UPDATER_TARGET_DIR is not set")
    path = Path(target_dir).expanduser()
    if not path.exists() or not path.is_dir():
        raise ConfigError(f"Target directory does not exist: {path}")
    return path


def resolve_target_exe(target_dir: Path) -> Path:
    exe_env = os.getenv("PC_UPDATER_TARGET_EXE")
    if exe_env:
        exe_path = Path(exe_env)
        if not exe_path.is_absolute():
            exe_path = target_dir / exe_path
        if not exe_path.exists():
            raise ConfigError(f"Configured exe not found: {exe_path}")
        return exe_path

    exes = [path for path in target_dir.iterdir() if path.suffix.lower() == ".exe"]
    if len(exes) == 1:
        return exes[0]
    if not exes:
        raise ConfigError("No .exe file found in target directory")
    raise ConfigError(
        "Multiple .exe files found; set PC_UPDATER_TARGET_EXE to choose one."
    )


def resolve_github_repo() -> str:
    repo = os.getenv("PC_UPDATER_GITHUB_REPO")
    if not repo:
        raise ConfigError("PC_UPDATER_GITHUB_REPO is not set")
    return repo


def resolve_autostart_name(exe_path: Path) -> str:
    return os.getenv("PC_UPDATER_AUTOSTART_NAME", exe_path.stem)


def get_temp_zip_path(prefix: str) -> Path:
    temp_dir = Path(tempfile.gettempdir())
    return temp_dir / f"{prefix}-{next(tempfile._get_candidate_names())}.zip"


def download_github_latest_release(repo: str, token: Optional[str]) -> Path:
    api_url = f"https://api.github.com/repos/{repo}/releases/latest"
    headers = {"Accept": "application/vnd.github+json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    response = requests.get(api_url, headers=headers, timeout=30)
    response.raise_for_status()
    release = response.json()
    assets = release.get("assets", [])
    zip_asset = next(
        (asset for asset in assets if asset.get("name", "").lower().endswith(".zip")),
        None,
    )
    if not zip_asset:
        raise RuntimeError("Latest release does not contain a .zip asset")

    download_url = zip_asset.get("browser_download_url")
    if not download_url:
        raise RuntimeError("Zip asset is missing download URL")

    temp_path = get_temp_zip_path("github-release")
    with requests.get(download_url, stream=True, timeout=60) as download:
        download.raise_for_status()
        with open(temp_path, "wb") as handle:
            for chunk in download.iter_content(chunk_size=1024 * 1024):
                if chunk:
                    handle.write(chunk)
    return temp_path


def extract_zip(zip_path: Path) -> Path:
    extract_dir = Path(tempfile.mkdtemp(prefix="pc-updater-"))
    with zipfile.ZipFile(zip_path, "r") as archive:
        archive.extractall(extract_dir)
    return extract_dir


def find_content_root(extract_dir: Path) -> Path:
    entries = list(extract_dir.iterdir())
    if len(entries) == 1 and entries[0].is_dir():
        return entries[0]
    return extract_dir


def collect_relative_paths(root: Path) -> Tuple[set[str], set[str]]:
    files: set[str] = set()
    directories: set[str] = set()
    for path in root.rglob("*"):
        relative = str(path.relative_to(root))
        if path.is_dir():
            directories.add(relative)
        else:
            files.add(relative)
            directories.add(str(path.parent.relative_to(root)))
    directories.discard(".")
    return files, directories


def remove_extraneous(target_dir: Path, files: set[str], directories: set[str]) -> None:
    for path in sorted(target_dir.rglob("*"), reverse=True):
        relative = str(path.relative_to(target_dir))
        if path.is_dir():
            if relative not in directories:
                shutil.rmtree(path, ignore_errors=True)
        else:
            if relative not in files:
                path.unlink(missing_ok=True)


def copy_updated_files(root: Path, target_dir: Path, files: Iterable[str]) -> None:
    for relative in files:
        source = root / relative
        destination = target_dir / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)


def sync_update(zip_path: Path, target_dir: Path) -> None:
    extract_dir = extract_zip(zip_path)
    try:
        content_root = find_content_root(extract_dir)
        files, directories = collect_relative_paths(content_root)
        remove_extraneous(target_dir, files, directories)
        for directory in sorted(directories):
            (target_dir / directory).mkdir(parents=True, exist_ok=True)
        copy_updated_files(content_root, target_dir, sorted(files))
    finally:
        shutil.rmtree(extract_dir, ignore_errors=True)


def stop_running_exe(exe_path: Path) -> None:
    if sys.platform != "win32":
        logger.warning("Process stop is only supported on Windows.")
        return
    try:
        subprocess.run(
            ["taskkill", "/F", "/IM", exe_path.name],
            check=False,
            capture_output=True,
        )
    except FileNotFoundError:
        logger.warning("taskkill not available; skipping process stop.")


def start_exe(exe_path: Path) -> None:
    if sys.platform != "win32":
        logger.warning("Process start is only supported on Windows.")
        return
    subprocess.Popen([str(exe_path)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def ensure_autostart(exe_path: Path, name: str) -> None:
    if sys.platform != "win32":
        logger.warning("Autostart registration is only supported on Windows.")
        return
    try:
        import winreg
    except ImportError:
        logger.warning("winreg not available; cannot update autostart.")
        return

    with winreg.OpenKey(
        winreg.HKEY_CURRENT_USER,
        r"Software\Microsoft\Windows\CurrentVersion\Run",
        0,
        winreg.KEY_SET_VALUE,
    ) as key:
        winreg.SetValueEx(key, name, 0, winreg.REG_SZ, str(exe_path))


def restart_and_autostart(target_dir: Path) -> None:
    exe_path = resolve_target_exe(target_dir)
    stop_running_exe(exe_path)
    start_exe(exe_path)
    ensure_autostart(exe_path, resolve_autostart_name(exe_path))


async def start_command(update: Update, context: ContextTypes.DEFAULT_TYPE) -> None:
    message = (
        "Доступные команды:\n"
        "/update_github — скачать последний релиз с GitHub и обновить.\n"
        "/update_zip — отправьте .zip файлом для обновления."
    )
    await update.message.reply_text(message)


async def update_from_github(update: Update, context: ContextTypes.DEFAULT_TYPE) -> None:
    await update.message.chat.send_action(ChatAction.TYPING)
    try:
        target_dir = resolve_target_dir()
        repo = resolve_github_repo()
        token = os.getenv("PC_UPDATER_GITHUB_TOKEN")
        await update.message.reply_text("Скачиваю последний релиз...")
        zip_path = download_github_latest_release(repo, token)
        try:
            await update.message.reply_text("Обновляю файлы...")
            sync_update(zip_path, target_dir)
            restart_and_autostart(target_dir)
        finally:
            zip_path.unlink(missing_ok=True)
        await update.message.reply_text("Обновление завершено и приложение перезапущено.")
    except Exception as exc:
        logger.exception("GitHub update failed")
        await update.message.reply_text(f"Ошибка при обновлении: {exc}")


async def update_zip_command(update: Update, context: ContextTypes.DEFAULT_TYPE) -> None:
    await update.message.reply_text("Отправьте .zip файлом в ответ на это сообщение.")
    context.chat_data["awaiting_zip"] = True


async def handle_zip(update: Update, context: ContextTypes.DEFAULT_TYPE) -> None:
    if not context.chat_data.get("awaiting_zip"):
        return
    document = update.message.document
    if not document.file_name.lower().endswith(".zip"):
        await update.message.reply_text("Нужен файл .zip. Попробуйте снова.")
        return

    context.chat_data["awaiting_zip"] = False
    await update.message.reply_text("Скачиваю файл...")

    try:
        target_dir = resolve_target_dir()
        file = await document.get_file()
        temp_path = get_temp_zip_path("chat-upload")
        await file.download_to_drive(custom_path=str(temp_path))
        await update.message.reply_text("Обновляю файлы...")
        sync_update(temp_path, target_dir)
        restart_and_autostart(target_dir)
        await update.message.reply_text("Обновление завершено и приложение перезапущено.")
    except Exception as exc:
        logger.exception("Zip update failed")
        await update.message.reply_text(f"Ошибка при обновлении: {exc}")
    finally:
        try:
            temp_path.unlink(missing_ok=True)
        except Exception:
            logger.warning("Failed to remove temp zip")


def main() -> None:
    token = os.getenv("TELEGRAM_BOT_TOKEN")
    if not token:
        raise ConfigError("TELEGRAM_BOT_TOKEN is not set")

    application = (
        ApplicationBuilder()
        .token(token)
        .build()
    )

    application.add_handler(CommandHandler("start", start_command))
    application.add_handler(CommandHandler("update_github", update_from_github))
    application.add_handler(CommandHandler("update_zip", update_zip_command))
    application.add_handler(
        MessageHandler(filters.Document.ALL & filters.ChatType.PRIVATE, handle_zip)
    )

    application.run_polling()


if __name__ == "__main__":
    try:
        main()
    except ConfigError as exc:
        logger.error(str(exc))
        raise SystemExit(1)
