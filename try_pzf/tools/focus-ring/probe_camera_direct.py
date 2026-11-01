"""Bounded direct TCP/WS probe. Prints lengths only; never saves camera images."""
import base64
import hashlib
import os
import socket
import struct
import sys
import time


def probe(host="10.23.38.143", port=80, path="/camera"):
    start = time.monotonic()
    with socket.create_connection((host, port), timeout=3) as sock:
        sock.settimeout(3)
        key = base64.b64encode(os.urandom(16)).decode()
        request = (f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\n"
                   "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                   f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
        sock.sendall(request.encode())
        buffered = bytearray()

        def take(count):
            while len(buffered) < count:
                chunk = sock.recv(max(4096, count-len(buffered)))
                if not chunk:
                    raise EOFError("peer closed TCP connection")
                buffered.extend(chunk)
            data = bytes(buffered[:count])
            del buffered[:count]
            return data

        while b"\r\n\r\n" not in buffered:
            chunk = sock.recv(4096)
            if not chunk:
                raise EOFError("peer closed before upgrade")
            buffered.extend(chunk)
            if len(buffered) > 8192:
                raise ValueError("HTTP header too large")
        end = buffered.index(b"\r\n\r\n") + 4
        headers = take(end).decode("latin1")
        expected = base64.b64encode(hashlib.sha1(
            (key+"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        if "101" not in headers.splitlines()[0] or expected not in headers:
            raise ValueError(headers.splitlines()[0])
        print("direct upgrade accepted", flush=True)
        for _ in range(4):
            first, second = take(2)
            length = second & 127
            if length == 126:
                length = struct.unpack("!H", take(2))[0]
            elif length == 127:
                length = struct.unpack("!Q", take(8))[0]
            if second & 128 or length > 2*1024*1024:
                raise ValueError("invalid server frame")
            payload = take(length)
            opcode = first & 15
            print(f"opcode={opcode} bytes={length} elapsed_ms={int((time.monotonic()-start)*1000)}", flush=True)
            if opcode == 9:
                mask = os.urandom(4)
                sock.sendall(bytes([0x8A, 0x80 | length])+mask+bytes(
                    value ^ mask[i % 4] for i, value in enumerate(payload)))
            if opcode == 8:
                break
        # Masked normal Close; the diagnostic never retains the camera slot.
        mask = os.urandom(4)
        payload = struct.pack("!H", 1000)
        sock.sendall(b"\x88\x82"+mask+bytes(v ^ mask[i % 4] for i, v in enumerate(payload)))


if __name__ == "__main__":
    try:
        probe(path=sys.argv[1] if len(sys.argv)>1 else "/camera")
    except Exception as error:
        print(type(error).__name__+": "+str(error))
