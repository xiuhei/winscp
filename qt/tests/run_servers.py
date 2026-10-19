import os
import signal
import subprocess
import sys
import time


def main():
    for name in ("sftp", "ftp", "ftps", "dav"):
        os.makedirs(f"/data/{name}", exist_ok=True)
    subprocess.run([sys.executable, "/tests/gen_cert.py", "/tmp/cert.pem", "/tmp/key.pem"], check=True)
    subprocess.run(["ssh-keygen", "-A"], check=True)
    with open("/etc/ssh/sshrc", "w") as handle:
        handle.write('printf "%s\\n" "$SSH_CONNECTION" > "$HOME/.terminal-connection"\n')
    commands = [
        [sys.executable, "/tests/ssh_test_server.py", "/data/sftp", "22222", "--host", "0.0.0.0"],
        [sys.executable, "/tests/ftp_test_server.py", "/data/ftp", "21212", "--host", "0.0.0.0"],
        [sys.executable, "/tests/ftp_test_server.py", "/data/ftps", "21213", "--host", "0.0.0.0",
         "--passive-port", "50010", "--tls", "/tmp/cert.pem", "/tmp/key.pem"],
        ["wsgidav", "--root", "/data/dav", "--host", "0.0.0.0", "--port", "18080", "--auth", "anonymous"],
        ["/usr/sbin/sshd", "-D", "-e", "-p", "22223", "-o", "PasswordAuthentication=yes",
         "-o", "UsePAM=no", "-o", "PrintMotd=no"],
    ]
    children = [subprocess.Popen(command) for command in commands]

    def stop(*_):
        for child in children:
            child.terminate()
        for child in children:
            child.wait()
        sys.exit(0)

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    while all(child.poll() is None for child in children):
        time.sleep(0.5)
    stop()


if __name__ == "__main__":
    main()
