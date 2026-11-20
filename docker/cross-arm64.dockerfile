# Toolchain di cross-compilazione per Raspberry Pi (aarch64) su host amd64.
FROM debian:bookworm

RUN dpkg --add-architecture arm64 \
 && printf 'deb [arch=amd64] http://deb.debian.org/debian bookworm main\n\
deb [arch=arm64] http://deb.debian.org/debian bookworm main\n\
deb [arch=amd64] http://deb.debian.org/debian-security bookworm-security main\n\
deb [arch=arm64] http://deb.debian.org/debian-security bookworm-security main\n\
deb [arch=amd64] http://deb.debian.org/debian bookworm-updates main\n\
deb [arch=arm64] http://deb.debian.org/debian bookworm-updates main\n' > /etc/apt/sources.list \
 && apt-get update -qq \
 && apt-get install -y --no-install-recommends g++-aarch64-linux-gnu cmake make libasound2-dev:arm64 \
 && rm -rf /var/lib/apt/lists/*
