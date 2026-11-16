#!/usr/bin/env bash
# Offline unit and loopback integration tests for Apple High Performance Screen Sharing,
# plus strict -Werror compile check of the portable ft-appless-cap CLI spike.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
mkdir -p "$root/build/tests"

ssl_cflags=""
ssl_libs="-lcrypto -lz"
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists openssl; then
    ssl_cflags=$(pkg-config --cflags openssl)
    ssl_libs="$(pkg-config --libs openssl) -lz"
elif [ -d /opt/homebrew/opt/openssl/include ]; then
    ssl_cflags="-I/opt/homebrew/opt/openssl/include"
    ssl_libs="-L/opt/homebrew/opt/openssl/lib -lcrypto -lz"
fi

# shellcheck disable=SC2086
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror $ssl_cflags \
    "$root/stream/tests/apple_hp_test.cpp" $ssl_libs -lpthread \
    -o "$root/build/tests/apple-hp-test"

# shellcheck disable=SC2086
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror $ssl_cflags \
    "$root/stream/spike/ft-appless-cap.cpp" $ssl_libs -lpthread \
    -o "$root/build/ft-appless-cap"

"$root/build/tests/apple-hp-test"
