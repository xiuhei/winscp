#!/usr/bin/env python3
"""Local SSH server providing SFTP, an interactive shell and the POSIX shell
commands used by the SCP backend. Used only by the WinSCP Qt integration tests.

Usage: ssh_test_server.py <root> <port>
"""
import argparse
import datetime
import os
import shlex
import socket
import stat
import sys
import threading
import time

import paramiko

ROOT = ""


def safe_path(path: str) -> str:
    global ROOT
    path = path.replace("\\", "/")
    if path in ("", "~", "."):
        return ROOT
    if path.startswith("~/"):
        path = path[2:]
    if not path.startswith("/"):
        path = "/" + path
    relative = path.lstrip("/")
    full = os.path.abspath(os.path.join(ROOT, relative))
    root_abs = os.path.abspath(ROOT)
    if not full.startswith(root_abs):
        full = root_abs
    return full


class StubSFTPServer(paramiko.SFTPServerInterface):
    def _real(self, path):
        return safe_path(path)

    def list_folder(self, path):
        target = self._real(path)
        result = []
        for name in sorted(os.listdir(target)):
            full = os.path.join(target, name)
            try:
                info = os.stat(full)
            except OSError:
                continue
            attr = paramiko.SFTPAttributes.from_stat(info, filename=name)
            result.append(attr)
        return result

    def stat(self, path):
        target = self._real(path)
        try:
            return paramiko.SFTPAttributes.from_stat(os.stat(target), filename=os.path.basename(target))
        except OSError as exc:
            return paramiko.SFTPServer.convert_errno(exc.errno)

    def lstat(self, path):
        target = self._real(path)
        try:
            return paramiko.SFTPAttributes.from_stat(os.lstat(target), filename=os.path.basename(target))
        except OSError as exc:
            return paramiko.SFTPServer.convert_errno(exc.errno)

    def open(self, path, flags, attr):
        target = self._real(path)
        try:
            fd = os.open(target, flags | getattr(os, "O_BINARY", 0), 0o644)
            file_obj = os.fdopen(fd, "r+b" if (flags & (os.O_WRONLY | os.O_RDWR)) else "rb")
        except OSError as exc:
            return paramiko.SFTPServer.convert_errno(exc.errno)
        handle = paramiko.SFTPHandle(flags)
        handle.filename = target
        handle.readfile = file_obj
        handle.writefile = file_obj
        return handle

    def remove(self, path):
        try:
            os.remove(self._real(path))
            return paramiko.SFTP_OK
        except OSError as exc:
            return paramiko.SFTPServer.convert_errno(exc.errno)

    def rename(self, oldpath, newpath):
        try:
            os.replace(self._real(oldpath), self._real(newpath))
            return paramiko.SFTP_OK
        except OSError as exc:
            return paramiko.SFTPServer.convert_errno(exc.errno)

    def mkdir(self, path, attr):
        try:
            os.mkdir(self._real(path), 0o755)
            return paramiko.SFTP_OK
        except OSError as exc:
            return paramiko.SFTPServer.convert_errno(exc.errno)

    def rmdir(self, path):
        try:
            os.rmdir(self._real(path))
            return paramiko.SFTP_OK
        except OSError as exc:
            return paramiko.SFTPServer.convert_errno(exc.errno)

    def chattr(self, path, attr):
        target = self._real(path)
        try:
            if attr.st_mode is not None:
                os.chmod(target, attr.st_mode)
            if attr.st_mtime is not None:
                os.utime(target, (attr.st_atime or attr.st_mtime, attr.st_mtime))
            return paramiko.SFTP_OK
        except OSError as exc:
            return paramiko.SFTPServer.convert_errno(exc.errno)

    def symlink(self, target_path, path):
        try:
            os.symlink(target_path, self._real(path))
            return paramiko.SFTP_OK
        except OSError as exc:
            return paramiko.SFTPServer.convert_errno(exc.errno)

    def readlink(self, path):
        try:
            return os.readlink(self._real(path))
        except OSError as exc:
            return paramiko.SFTPServer.convert_errno(exc.errno)


def mode_string(info: os.stat_result) -> str:
    mode = stat.filemode(info.st_mode)
    return mode


def format_ls(path: str, long_format: bool) -> str:
    target = safe_path(path)
    if os.path.isfile(target):
        entries = [(target, os.path.basename(path) or path)]
    else:
        entries = [(os.path.join(target, name), name) for name in sorted(os.listdir(target))]
    lines = []
    if long_format and os.path.isdir(target):
        lines.append("total %d" % len(entries))
    for full, name in entries:
        try:
            info = os.lstat(full)
        except OSError:
            continue
        if not long_format:
            lines.append(name)
            continue
        dt = datetime.datetime.fromtimestamp(info.st_mtime)
        date = dt.strftime("%Y-%m-%d %H:%M")
        suffix = " -> " + os.readlink(full) if os.path.islink(full) else ""
        lines.append(f"{mode_string(info)} {1:2d} test test {info.st_size:8d} {date} {name}{suffix}")
    return "\n".join(lines) + ("\n" if lines else "")


def execute_command(command: str, channel, env):
    """Emulate the small POSIX subset used by ScpBackend."""
    time.sleep(0.6)
    print(f"RUN {command!r}", flush=True)
    command = command.strip()
    try:
        parts = shlex.split(command)
    except ValueError as exc:
        channel.sendall_stderr(f"parse error: {exc}\n")
        channel.send_exit_status(1)
        return
    if not parts:
        channel.send_exit_status(0)
        channel.close()
        return
    if parts[0] == "env":
        parts = parts[1:]
        while parts and "=" in parts[0]:
            parts = parts[1:]
    if not parts:
        channel.send_exit_status(0)
        channel.close()
        return
    program, args = parts[0], parts[1:]

    if program == "cat" and ">" in args:
        target = safe_path(args[args.index(">") + 1])
        data = b""
        while True:
            chunk = channel.recv(65536)
            if not chunk:
                break
            data += chunk
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, "wb") as handle:
            handle.write(data)
        channel.send_exit_status(0)
        channel.close()
        return

    def strip_dashdash(items):
        return [item for item in items if item != "--"]

    stdout = ""
    status = 0
    try:
        if program == "pwd":
            stdout = "/\n"
        elif program == "cd":
            stdout = ""
        elif program == "ls":
            long_format = any(arg.startswith("-") and "l" in arg for arg in args)
            positional = [arg for arg in strip_dashdash(args) if not arg.startswith("-")]
            target = positional[0] if positional else "/"
            stdout = format_ls(target, long_format)
        elif program == "cat":
            positional = strip_dashdash(args)
            target = safe_path(positional[0] if positional else "")
            if not os.path.isfile(target):
                raise FileNotFoundError(target)
            with open(target, 'rb') as handle:
                payload = handle.read()
            print(f'CAT {len(payload)}', flush=True)
            channel.sendall(payload)
            print('CAT-SENT', flush=True)
            channel.send_exit_status(0)
            channel.close()
            return
        elif program == "mkdir":
            for arg in strip_dashdash(args):
                if not arg.startswith("-"):
                    os.makedirs(safe_path(arg), exist_ok=True)
        elif program == "rm":
            recursive = any(arg.startswith("-") and "r" in arg for arg in args)
            force = any(arg.startswith("-") and "f" in arg for arg in args)
            for arg in strip_dashdash(args):
                if arg.startswith("-"):
                    continue
                target = safe_path(arg)
                if os.path.isdir(target):
                    if recursive:
                        import shutil

                        shutil.rmtree(target)
                    else:
                        os.rmdir(target)
                elif os.path.exists(target):
                    os.remove(target)
                elif not force:
                    raise FileNotFoundError(target)
        elif program == "mv":
            positional = [arg for arg in strip_dashdash(args) if not arg.startswith("-")]
            if len(positional) >= 2:
                os.replace(safe_path(positional[0]), safe_path(positional[1]))
        elif program == "chmod":
            positional = [arg for arg in strip_dashdash(args) if not arg.startswith("-")]
            if len(positional) >= 2:
                os.chmod(safe_path(positional[1]), int(positional[0], 8))
        elif program == "touch":
            positional = [arg for arg in strip_dashdash(args) if not arg.startswith("-")]
            if "-d" in args:
                index = args.index("-d")
                stamp = args[index + 1].lstrip("@")
                timestamp = int(stamp) if stamp.isdigit() else time.time()
                for arg in positional:
                    target = safe_path(arg)
                    open(target, "a").close()
                    os.utime(target, (timestamp, timestamp))
            elif "-t" in args:
                index = args.index("-t")
                parsed = datetime.datetime.strptime(args[index + 1], "%Y%m%d%H%M.%S").timestamp()
                for arg in positional:
                    target = safe_path(arg)
                    open(target, "a").close()
                    os.utime(target, (parsed, parsed))
            else:
                for arg in positional:
                    open(safe_path(arg), "a").close()
        elif program == "ln":
            positional = [arg for arg in strip_dashdash(args) if not arg.startswith("-")]
            if len(positional) >= 2:
                target = positional[0]
                link = safe_path(positional[1])
                if os.path.lexists(link):
                    os.remove(link)
                os.symlink(target, link)
        else:
            status = 127
            channel.sendall_stderr(f"{program}: not found\n")
    except Exception as exc:  # noqa: BLE001 - test server
        status = 1
        channel.sendall_stderr(f"{exc}\n")
    if stdout:
        print(f'SEND {len(stdout)} bytes closed={channel.closed}', flush=True)
        channel.sendall(stdout.encode('utf-8'))
        print('SENT', flush=True)
    channel.send_exit_status(status)
    print(f'STATUS {status}', flush=True)
    channel.close()


def handle_shell(channel):
    channel.sendall(b"\x1b[1;32mWelcome to WinSCP Qt test SSH server\x1b[0m\r\n")
    channel.sendall(b"\x1b[90mType 'help' for available commands.\x1b[0m\r\n")
    prompt = b"\x1b[1;32mtest@winscp\x1b[0m:\x1b[1;34m~\x1b[0m$ "
    channel.sendall(prompt)
    buffer = b""
    cwd = "/"
    while True:
        data = channel.recv(4096)
        if not data:
            break
        for byte in data:
            char = bytes([byte])
            if char in (b"\r", b"\n"):
                channel.sendall(b"\r\n")
                line = buffer.decode("utf-8", "replace").strip()
                buffer = b""
                if line in ("exit", "logout"):
                    channel.sendall(b"Goodbye.\r\n")
                    channel.send_exit_status(0)
                    channel.close()
                    return
                elif line == "help":
                    channel.sendall(b"ls, pwd, echo <text>, cd <dir>, whoami, uname, exit\r\n")
                elif line.startswith("echo "):
                    channel.sendall((line[5:] + "\r\n").encode())
                elif line == "connection-id":
                    identity = channel.get_transport().session_id.hex()
                    channel.sendall(("CONNECTION:" + identity + "\r\n").encode())
                elif line == "pwd":
                    channel.sendall((cwd + "\r\n").encode())
                elif line == "whoami":
                    channel.sendall(b"test\r\n")
                elif line == "uname":
                    channel.sendall(b"Linux winscp-test 6.8.0 x86_64 GNU/Linux\r\n")
                elif line == "ls":
                    channel.sendall(format_ls(cwd, True).encode())
                elif line.startswith("cd "):
                    target = line[3:].strip()
                    cwd = "/" if target in ("", "~") else target
                elif line:
                    channel.sendall((line + ": command not found\r\n").encode())
                channel.sendall(prompt)
            elif char in (b"\x7f", b"\b"):
                if buffer:
                    buffer = buffer[:-1]
                    channel.sendall(b"\b \b")
            elif char == b"\x03":
                channel.sendall(b"^C\r\n")
                buffer = b""
                channel.sendall(prompt)
            else:
                try:
                    text = char.decode("utf-8")
                except UnicodeDecodeError:
                    continue
                if text.isprintable():
                    buffer += char
                    channel.sendall(char)


class Server(paramiko.ServerInterface):
    def __init__(self):
        self.event = threading.Event()

    def check_auth_password(self, username, password):
        if username == "test" and password == "test":
            with open(os.path.join(ROOT, ".terminal-connection"), "w") as handle:
                handle.write(self.transport.session_id.hex())
            return paramiko.AUTH_SUCCESSFUL
        return paramiko.AUTH_FAILED

    def get_allowed_auths(self, username):
        return "password"

    def check_channel_request(self, kind, chanid):
        if kind == "session":
            return paramiko.OPEN_SUCCEEDED
        return paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED

    def check_channel_subsystem_request(self, channel, name):
        return super().check_channel_subsystem_request(channel, name)

    def check_channel_pty_request(self, channel, term, width, height, pixelwidth, pixelheight,
                                  modes):
        return True

    def check_channel_shell_request(self, channel):
        threading.Thread(target=handle_shell, args=(channel,), daemon=True).start()
        return True

    def check_channel_exec_request(self, channel, command):
        print(f"EXEC {command!r}", flush=True)
        command_text = command.decode("utf-8", "replace")
        threading.Thread(target=execute_command, args=(command_text, channel, {}), daemon=True).start()
        return True


def main() -> int:
    global ROOT
    parser = argparse.ArgumentParser()
    parser.add_argument("root")
    parser.add_argument("port", type=int)
    parser.add_argument("--host", default="127.0.0.1")
    args = parser.parse_args()
    os.makedirs(args.root, exist_ok=True)
    ROOT = os.path.abspath(args.root)

    host_key_path = os.path.join(os.path.dirname(__file__), "test_host_key")
    if not os.path.exists(host_key_path):
        key = paramiko.RSAKey.generate(2048)
        key.write_private_key_file(host_key_path)
    host_key = paramiko.RSAKey(filename=host_key_path)

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.host, args.port))
    sock.listen(20)
    print(f"READY ssh://test:test@127.0.0.1:{args.port} root={ROOT}", flush=True)

    transports = []
    while True:
        client, _ = sock.accept()
        transport = paramiko.Transport(client)
        transport.add_server_key(host_key)
        transport.set_subsystem_handler("sftp", paramiko.SFTPServer, StubSFTPServer)
        server = Server()
        server.transport = transport
        try:
            transport.start_server(server=server)
        except Exception:
            continue
        transports.append(transport)
        transports = [item for item in transports if item.is_active()]


if __name__ == "__main__":
    sys.exit(main())






