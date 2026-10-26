"""Automatically collect cloud TTS replies, without text injection or synthesis."""
from __future__ import annotations

import argparse
from datetime import datetime
from functools import lru_cache
import json
from pathlib import Path
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from tts_capture_format import MAX_EXPORT_BYTES, convert_wav, find_ffmpeg, make_ogg, opus_samples_48k, parse_capture

CONTROLLER = Path(__file__).resolve().parents[1]


class CaptureClient:
    def __init__(self, host: str):
        if "://" not in host:
            host = "http://" + host
        parsed = urllib.parse.urlsplit(host)
        if parsed.scheme != "http" or not parsed.hostname or parsed.username or parsed.password or parsed.path not in ("", "/") or parsed.query or parsed.fragment:
            raise ValueError("--host must be the controller IP/hostname, optionally with :port")
        self.url = host.rstrip("/") + "/api/tts/capture"
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        self.owned_id = None

    def request(self, action=None, query="", binary=False):
        body = None if action is None else json.dumps({"action": action}).encode("utf-8")
        request = urllib.request.Request(self.url + query, data=body,
                                        headers={"Content-Type": "application/json", "Connection": "close"})
        limit = MAX_EXPORT_BYTES if binary else 20000
        with self.opener.open(request, timeout=30) as response:
            data = response.read(limit + 1)
        if len(data) > limit:
            raise ValueError("Response exceeds capture limit")
        return data if binary else json.loads(data.decode("utf-8"))

    def arm(self, deadline):
        while time.monotonic() < deadline:
            try:
                status = self.request("arm")
                self.owned_id = int(status["id"])
                return self.owned_id
            except urllib.error.HTTPError as error:
                if error.code != 409:
                    raise
                if self.request()["state"] in ("armed", "recording"):
                    raise RuntimeError("Another capture is active; stop that tool first") from error
                time.sleep(0.5)
        raise TimeoutError("Controller remained busy; no recording was armed")

    def reply(self, capture_id, deadline):
        while time.monotonic() < deadline:
            status = self.request()
            if int(status["id"]) != capture_id:
                raise RuntimeError("Capture ID changed: another process changed the recording")
            state = status["state"]
            if state == "error":
                raise RuntimeError("Board capture failed: " + status.get("error", "unknown"))
            if state == "complete":
                data = self.request(query=f"?download=1&id={capture_id}", binary=True)
                _, packets = parse_capture(data)
                if len(packets) != status["frames"]:
                    raise ValueError("Downloaded packet count differs from metadata")
                # Recheck after download in case a concurrent callback reported
                # contention while STOP was sealing the final packet buffer.
                final = self.request()
                if final["id"] != capture_id or final["state"] != "complete":
                    raise RuntimeError("Capture became invalid during download")
                return status, data
            if state not in ("armed", "recording", "sealing"):
                raise RuntimeError("Recording was cancelled")
            time.sleep(0.25)
        raise TimeoutError("No complete TTS reply before timeout")

    def close(self):
        if self.owned_id is None:
            return
        try:
            status = self.request()
            if int(status["id"]) == self.owned_id:
                self.request("cancel")
        finally:
            self.owned_id = None


def normalized(text):
    text = "".join(char for char in text.casefold() if char.isalnum())
    return text.replace("福克斯", "focus")


@lru_cache(maxsize=1)
def capture_phrases():
    """Explicit approved alternatives, not an unbounded fuzzy/LLM matcher."""
    path = Path(__file__).with_name("tts_capture_phrases.json")
    phrases = json.loads(path.read_text(encoding="utf-8-sig"))["phrases"]
    expected_names = [f"welcome_{index:02d}.wav" for index in range(1, 10)] + ["sedentary_reminder.wav"]
    if not isinstance(phrases, list) or [item.get("file") for item in phrases] != expected_names:
        raise ValueError("Capture phrases must list welcome_01..09 and sedentary_reminder in order")
    for item in phrases:
        if not isinstance(item.get("text"), str) or not normalized(item["text"]):
            raise ValueError("Capture prompt must be nonempty text")
        accepted = item.get("accepted", [])
        if not isinstance(accepted, list) or any(not isinstance(text, str) or not normalized(text) for text in accepted):
            raise ValueError("Approved alternatives must be nonempty strings")
    return phrases


def match_kind(status, expected):
    if status.get("text_truncated") or not expected:
        return "none"
    actual, target = normalized(status.get("cloud_text", "")), normalized(expected)
    if not actual or not target:
        return "none"
    if actual == target:
        return "exact"
    for phrase in capture_phrases():
        approved = {normalized(text) for text in [phrase["text"], *phrase.get("accepted", [])]}
        if target in approved and actual in approved:
            return "equivalent"
    return "none"


def text_matches(status, expected):
    return match_kind(status, expected) != "none"


def batch_targets():
    targets = [(item["file"], item["text"]) for item in capture_phrases()]
    if len(targets) != 10 or any(not re.fullmatch(r"(?:welcome_0[1-9]|sedentary_reminder)\.wav", name) for name, _ in targets):
        raise ValueError("Expected the existing nine welcome files and one reminder")
    return targets


def save_reply(folder, name, status, data, ffmpeg, expected=""):
    rate, packets = parse_capture(data)
    stem = Path(name).stem
    raw_path, opus_path = folder / (stem + ".fop"), folder / (stem + ".opus")
    metadata_path = folder / (stem + ".json")
    for path in (raw_path, opus_path, metadata_path, folder / name):
        if path.exists():
            raise FileExistsError("Refusing to overwrite: " + str(path))
    with raw_path.open("xb") as output:
        output.write(data)
    with opus_path.open("xb") as output:
        output.write(make_ogg(packets, rate))
    metadata = dict(status, expected_text=expected, transcript_matches=text_matches(status, expected),
                    match_type=match_kind(status, expected),
                    source="cloud_tts_opus", audible_verified=False)
    result = opus_path
    if ffmpeg:
        result = folder / name
        expected_samples = sum(opus_samples_48k(packet) for packet in packets) // 3
        metadata["duration_seconds"] = convert_wav(ffmpeg, opus_path, result, expected_samples)
    with metadata_path.open("x", encoding="utf-8") as output:
        json.dump(metadata, output, ensure_ascii=False, indent=2)
    return result


def run(args):
    ffmpeg = find_ffmpeg(args.ffmpeg)
    if not ffmpeg and not args.opus_only:
        raise RuntimeError("FFmpeg required for WAV. Run Capture-XiaozhiVoice.ps1, specify --ffmpeg PATH, or use --opus-only.")
    folder = Path(args.output) if args.output else CONTROLLER / "audio_captures" / datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    asset_dir = (CONTROLLER / "main/audio").resolve()
    if folder.resolve() == asset_dir or asset_dir in folder.resolve().parents:
        raise ValueError("Output must not be inside production main/audio")
    folder.mkdir(parents=True, exist_ok=False)
    client = CaptureClient(args.host)
    start_at = getattr(args, "start_at", 1)
    if not 1 <= start_at <= 10 or (not args.batch and start_at != 1):
        raise ValueError("--start-at must be 1..10 and requires --batch when not 1")
    targets = batch_targets()[start_at - 1:] if args.batch else [(f"capture_{index:02d}.wav", "") for index in range(1, args.count + 1)]
    print("只采集云端对话，拍头本地录音不会被采集。按 Ctrl+C 停止。", flush=True)
    print("输出目录：" + str(folder.resolve()), flush=True)
    if args.batch:
        print("接受文案表中列明的近义/同音说法；无关问候、告别和残句仍会另存。", flush=True)
    try:
        for index, (name, text) in enumerate(targets, start_at if args.batch else 1):
            deadline, attempt = time.monotonic() + args.timeout, 0
            while True:
                attempt += 1
                capture_id = client.arm(deadline)
                if text:
                    print(f"\n[{index}/10] 已准备：{name}\n请让小智只朗读：{text}", flush=True)
                else:
                    print(f"\n[{index}/{len(targets)}] 已准备，等待下一次完整云端回复……", flush=True)
                status, data = client.reply(capture_id, deadline)
                if not text or text_matches(status, text):
                    result = save_reply(folder, name, status, data, None if args.opus_only else ffmpeg, text)
                    if text and match_kind(status, text) == "equivalent":
                        print("已接受近义表达（保留实际字幕）。", flush=True)
                    print("已保存：" + str(result) + "\n云端字幕：" + status.get("cloud_text", ""), flush=True)
                    break
                other = folder / "unmatched"
                other.mkdir(exist_ok=True)
                other_name = f"{Path(name).stem}_attempt_{attempt:02d}.wav"
                save_reply(other, other_name, status, data, None if args.opus_only else ffmpeg, text)
                print("字幕不匹配，已另存 unmatched，不计入完成。实际：" + status.get("cloud_text", ""), flush=True)
    finally:
        try:
            client.close()
        except Exception as error:
            print("未能释放板端缓存：" + str(error), file=sys.stderr)
    print("\n采集完成。请逐条试听；没有覆盖生产录音，也没有自动烧录。", flush=True)
    return folder


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except AttributeError:
        pass
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="controller IP, not head IP")
    parser.add_argument("--batch", action="store_true", help="collect nine welcomes + reminder with subtitle checks")
    parser.add_argument("--count", type=int, default=1)
    parser.add_argument("--start-at", type=int, default=1, help="resume batch at original phrase number 1..10; old files remain untouched")
    parser.add_argument("--timeout", type=float, default=180, help="seconds per target, including unmatched replies")
    parser.add_argument("--output", help="new output directory")
    parser.add_argument("--ffmpeg", help="existing ffmpeg.exe")
    parser.add_argument("--opus-only", action="store_true", help="save Opus without WAV conversion")
    args = parser.parse_args()
    if args.count < 1 or args.timeout <= 0:
        parser.error("count and timeout must be positive")
    try:
        run(args)
    except KeyboardInterrupt:
        print("\n已停止，已有文件保留。", file=sys.stderr)
        return 130
    except Exception as error:
        print("采集失败：" + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
