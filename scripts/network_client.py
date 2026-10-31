#!/usr/bin/env python3
"""Small EC1 client. Commands omit the EC1/rid prefix unless --raw is used."""

import argparse
import sys

from network_common import FrameClient


def stdin_commands(max_characters: int = 4096):
    while True:
        line = sys.stdin.readline(max_characters + 2)
        if not line:
            return
        command = line.rstrip("\n")
        if len(command) > max_characters:
            raise ValueError(f"stdin command exceeds {max_characters} characters")
        yield command


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--command", dest="commands", action="append", default=[],
                        help='e.g. "LIMIT 1 SELL 10 24000"')
    parser.add_argument("--raw", action="store_true", help="send complete EC1 lines with explicit request IDs")
    parser.add_argument("command", nargs="*", help="one command; otherwise read commands from stdin")
    args = parser.parse_args()
    commands = args.commands.copy()
    if args.command:
        commands.append(" ".join(args.command))
    source = commands if commands else stdin_commands()
    try:
        with FrameClient(args.host, args.port, timeout=args.timeout) as client:
            for command in source:
                if not command.strip():
                    continue
                if args.raw:
                    client.send_raw(command)
                    response = client.receive()
                else:
                    response = client.request(command)
                print("\n".join(response.lines()), flush=True)
        return 0
    except (OSError, RuntimeError, ValueError) as error:
        print(f"network client: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
