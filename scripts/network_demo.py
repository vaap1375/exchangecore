#!/usr/bin/env python3
"""Verified two-client, partial-fill EC1 demonstration on an ephemeral local port."""

import argparse
from pathlib import Path
import sys

from network_common import FrameClient, ManagedServer, Response


def require(response: Response, kind: str, fields: tuple[str, ...] | None = None) -> None:
    if response.sequence == 0 or response.kind != kind:
        raise RuntimeError(f"expected sequenced {kind}, got {response.lines()}")
    if fields is not None and response.records[0] != (kind, *fields):
        raise RuntimeError(f"unexpected response: {response.lines()}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", "--server", dest="binary", type=Path,
                        default=Path(__file__).resolve().parents[1] / "build/release/exchangecore_gateway")
    args = parser.parse_args()
    try:
        with ManagedServer(args.binary) as server:
            print(f"Server listening on {server.host}:{server.port}")
            with FrameClient(server.host, server.port) as seller, FrameClient(server.host, server.port) as buyer:
                last_sequence = 0

                def exchange(client: FrameClient, name: str, command: str) -> Response:
                    nonlocal last_sequence
                    result = client.request(command)
                    if result.sequence <= last_sequence:
                        raise RuntimeError("sequential demo requests lost processing order")
                    last_sequence = result.sequence
                    print(f"\n{name} -> {command}\n" + "\n".join(result.lines()))
                    return result

                require(exchange(seller, "seller", "LIMIT 101 SELL 10 24000"), "OK",
                        ("101", "0", "10", "0", "0"))
                fill = exchange(buyer, "buyer", "LIMIT 102 BUY 4 24000")
                require(fill, "OK", ("102", "4", "0", "0", "1"))
                if fill.records[1:] != (("TRADE", "101", "102", "24000", "4"),):
                    raise RuntimeError("cross-client partial fill was routed incorrectly")
                require(exchange(buyer, "buyer", "BEST"), "BEST", ("NONE", "24000", "1", "6"))
                require(exchange(buyer, "buyer", "MARKET 103 BUY 2"), "OK",
                        ("103", "2", "0", "0", "1"))
                require(exchange(seller, "seller", "LIMIT 104 BUY 3 23999"), "OK",
                        ("104", "0", "3", "0", "0"))
                require(exchange(seller, "seller", "CANCEL 104"), "OK",
                        ("104", "0", "0", "3", "0"))
                require(exchange(buyer, "buyer", "BEST"), "BEST", ("NONE", "24000", "1", "4"))
                require(exchange(seller, "seller", "CANCEL 101"), "OK",
                        ("101", "0", "0", "4", "0"))
                require(exchange(buyer, "buyer", "BEST"), "BEST", ("NONE", "NONE", "0", "0"))
        print(f"\nClean shutdown. STATS {server.stats}")
        if "accepted" not in server.stats or server.stats.get("accepted") != server.stats.get("processed"):
            raise RuntimeError("shutdown lost admitted requests")
        return 0
    except (OSError, RuntimeError, ValueError) as error:
        print(f"network demo: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
