#!/usr/bin/env python3
"""Receives the game's live log (cvar log_network = "HOST:PORT") over TCP and appends it to a file.

  python3 tools/console-test/live_log.py PORT OUTPUT.log

Stops with Ctrl-C (or when killed). One connection at a time; a new connection (the game restarted) is appended.
Portable replacement for `nc -l`, whose options differ between macOS and Linux.
"""
import socket
import sys


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    port, path = int(sys.argv[1]), sys.argv[2]
    server = socket.socket()
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("", port))
    server.listen(1)
    with open(path, "ab", buffering=0) as out:
        while True:
            connection, _ = server.accept()
            with connection:
                while True:
                    data = connection.recv(65536)
                    if not data:
                        break
                    out.write(data)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
