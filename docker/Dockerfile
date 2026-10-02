FROM debian:12

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential clang cmake ninja-build gdb valgrind \
    libssl-dev nlohmann-json3-dev tcpdump iproute2 netcat-openbsd curl ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
CMD ["sleep", "infinity"]