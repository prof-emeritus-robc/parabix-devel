#!/bin/bash
#  Part of the Parabix Project, under the Open Software License 3.0.
#  SPDX-License-Identifier: OSL-3.0
#
#  SessionStart hook for Claude Code cloud sessions: installs the build
#  dependencies of Parabix (LLVM >= 16 development files, Boost, Z3),
#  configures a Release build in build/ and builds the LDML tools
#  (ldml_trules, tconv).  Idempotent: packages already installed are not
#  reinstalled, and make rebuilds only what changed.
set -euo pipefail

if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  exit 0
fi

cd "$CLAUDE_PROJECT_DIR"

packages=(cmake g++ python3 llvm-18-dev libboost-filesystem-dev libboost-iostreams-dev
          libboost-regex-dev libz3-dev zlib1g-dev libzstd-dev)
missing=()
for p in "${packages[@]}"; do
  dpkg -s "$p" >/dev/null 2>&1 || missing+=("$p")
done
if [ ${#missing[@]} -gt 0 ]; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get install -y -q "${missing[@]}" || { apt-get update -q && apt-get install -y -q "${missing[@]}"; }
fi

mkdir -p build "$HOME/.cache"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLLVM_PATH=/usr/lib/llvm-18 > build/cmake.log
make -C build -j"$(nproc)" ldml_trules tconv > build/make.log
