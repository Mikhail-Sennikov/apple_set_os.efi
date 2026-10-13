FROM debian:trixie-slim

WORKDIR /build

RUN apt-get update && apt-get install -y \
  build-essential \
  git \
  gnu-efi

CMD make clean && make
