#!/usr/bin/env python3
"""The parity runner's remote-admin client: the client half of the game's
remote-admin TCP protocol as engine/net/admin/admin_protocol.{h,cpp} has it
(the 0x0A0D0000 marker and u32 total length, the 33-byte challenge, the
65-byte login encrypted under the challenge string, NUL-terminated commands).
Standard library only.

    python -I admin_client.py query <host> <port> [--timeout S]
    python -I admin_client.py run <host> <port> <user> <password> <command> [<command> ...]
        [--quiet S] [--timeout S]

`query` sends QUERY in place of the login on a connection of its own; the
server answers with its plaintext status and closes. `run` logs in and sends
each command, reading its replies until the server stays quiet for --quiet
seconds. Both print one JSON object on stdout: each reply's text byte for byte
(CR LF included, Latin-1) with its receive stamp, or the error that ended the
exchange. The exit status is 0 when the exchange completed, 1 otherwise.
"""
import json
import socket
import struct
import sys
import time

MARKER = 0x0A0D0000
LCG = 78665521


def packet(payload):
    return struct.pack("<II", MARKER, 8 + len(payload)) + payload


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("closed")
        buf += chunk
    return buf


def recv_packet(sock):
    marker, total = struct.unpack("<II", recv_exact(sock, 8))
    if marker != MARKER or total < 8:
        raise ValueError(f"bad header {marker:#x} {total}")
    return recv_exact(sock, total - 8)


def cipher_words(key):
    seed = 0
    for i, b in enumerate(key):
        v = b - 256 if b >= 128 else b
        seed = (seed + v * v + i) & 0xFFFFFFFF
    seed = (seed + len(key) + 0x32) & 0xFFFFFFFF
    s1 = (seed * LCG + 1) & 0xFFFF
    s2 = (LCG * s1 + 1) & 0xFFFF
    s3 = (LCG * s2 + 1) & 0xFFFF
    return s1, s2, s3


def encode_login(challenge, user, password):
    buf = bytearray(65)
    u, p = user.encode("latin-1")[:31], password.encode("latin-1")[:31]
    buf[0:len(u)] = u
    buf[32:32 + len(p)] = p
    key = challenge.split(b"\0", 1)[0]
    if not key:
        return bytes(buf)
    s1, s2, s3 = cipher_words(key)
    for i in range(len(buf)):
        buf[i] = (buf[i] + key[i % len(key)]) & 0xFF
    if s3 & 1:
        buf.reverse()
    add, step = s1 & 0xFF, s2 & 0xFF
    for i in range(len(buf)):
        buf[i] = (buf[i] + i + add) & 0xFF
        add = (add + step) & 0xFF
    state = s3
    for i in range(len(buf)):
        state = (LCG * state + 1) & 0xFFFF
        buf[i] = (buf[i] + (state & 0xFF)) & 0xFF
    return bytes(buf)


def read_replies(sock, quiet):
    out = []
    sock.settimeout(quiet)
    while True:
        try:
            payload = recv_packet(sock)
        except socket.timeout:
            break
        except ConnectionError:
            out.append({"t": time.time(), "closed": True})
            break
        out.append({"t": time.time(), "text": payload.split(b"\0", 1)[0].decode("latin-1")})
    return out


def take_option(argv, name, default):
    if name in argv:
        k = argv.index(name)
        value = float(argv[k + 1])
        del argv[k:k + 2]
        return value
    return default


def main(argv):
    quiet = take_option(argv, "--quiet", 1.5)
    timeout = take_option(argv, "--timeout", 10.0)
    if len(argv) < 3 or argv[0] not in ("query", "run") or (argv[0] == "run" and len(argv) < 6):
        print(__doc__, file=sys.stderr)
        return 2
    mode, host, port = argv[0], argv[1], int(argv[2])
    result = {"mode": mode, "host": host, "port": port, "start": time.time()}
    try:
        sock = socket.create_connection((host, port), timeout=timeout)
        try:
            sock.settimeout(timeout)
            challenge = recv_packet(sock)
            result["challenge_ok"] = len(challenge) == 33 and challenge[-1] == 0
            if mode == "query":
                sock.sendall(packet(b"QUERY\0"))
                result["replies"] = read_replies(sock, quiet)
            else:
                user, password, commands = argv[3], argv[4], argv[5:]
                sock.sendall(packet(encode_login(challenge, user, password)))
                result["login"] = read_replies(sock, quiet)
                result["steps"] = []
                for command in commands:
                    sent = time.time()
                    sock.sendall(packet(command.encode("latin-1") + b"\0"))
                    replies = read_replies(sock, quiet)
                    result["steps"].append({"cmd": command, "sent": sent, "replies": replies})
                    if replies and replies[-1].get("closed"):
                        break
        finally:
            sock.close()
    except (OSError, ValueError, ConnectionError) as error:
        result["error"] = f"{type(error).__name__}: {error}"
    print(json.dumps(result))
    return 1 if "error" in result else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
