FROM python:3.13-slim
RUN apt-get update && apt-get install -y --no-install-recommends openssh-server vim-tiny procps \
    && rm -rf /var/lib/apt/lists/* \
    && pip install --no-cache-dir paramiko pyftpdlib pyopenssl cryptography wsgidav cheroot \
    && useradd -m -s /bin/bash test && echo 'test:test' | chpasswd \
    && mkdir -p /run/sshd
COPY . /tests
ENV PYTHONUNBUFFERED=1
WORKDIR /tests
