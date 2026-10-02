FROM debian:bookworm-slim

WORKDIR /build

RUN apt-get update && apt-get install -y \
  build-essential \
  gnu-efi \
  && rm -rf /var/lib/apt/lists/*

CMD ["sh", "-c", "make clean && make test && make && make DEBUG=1"]
