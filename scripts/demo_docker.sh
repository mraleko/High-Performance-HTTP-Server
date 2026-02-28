#!/usr/bin/env bash
set -euo pipefail

docker run --rm -it -v "$PWD":/work -w /work ubuntu:24.04 bash -lc 'apt-get update >/dev/null && DEBIAN_FRONTEND=noninteractive apt-get install -y build-essential make python3 curl >/dev/null && make clean && bash scripts/demo_linux.sh'
