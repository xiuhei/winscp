#!/usr/bin/env python3
"""Local FTP/FTPS server used by the WinSCP Qt integration tests.

Usage: ftp_test_server.py <root> <port> [--tls <certfile> <keyfile>]
"""
import argparse
import os
import sys

from pyftpdlib.authorizers import DummyAuthorizer
from pyftpdlib.handlers import FTPHandler
from pyftpdlib.servers import FTPServer
import logging
from pyftpdlib.log import config_logging
config_logging(level=logging.DEBUG)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root")
    parser.add_argument("port", type=int)
    parser.add_argument("--tls", nargs=2, metavar=("CERT", "KEY"))
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--passive-port", type=int, default=50000)
    args = parser.parse_args()

    os.makedirs(args.root, exist_ok=True)
    authorizer = DummyAuthorizer()
    authorizer.add_user("test", "test", args.root, perm="elradfmwMT")

    if args.tls:
        from pyftpdlib.handlers.ftps.control import TLS_FTPHandler

        handler = TLS_FTPHandler
        handler.certfile = args.tls[0]
        handler.keyfile = args.tls[1]
        handler.tls_control_required = True
        handler.tls_data_required = True
    else:
        handler = FTPHandler
    handler.authorizer = authorizer
    handler.passive_ports = range(args.passive_port, args.passive_port + 10)
    handler.masquerade_address = "127.0.0.1"

    server = FTPServer((args.host, args.port), handler)
    print(f"READY ftp://127.0.0.1:{args.port} root={args.root}", flush=True)
    server.serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())


