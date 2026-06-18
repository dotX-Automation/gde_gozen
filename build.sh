#!/bin/bash

# ==== REQUIREMENTS ====
# Requirements for a x86_64 Ubuntu 24.04.4 machine
#
# sudo dpkg --add-architecture arm64
# sudo apt install scons cmake make aom-tools libaom-dev libvpx-dev yasm nasm mingw-w64 gcc-14-aarch64-linux-gnu g++-14-aarch64-linux-gnu crossbuild-essential-arm64 zlib1g-dev zlib1g-dev:arm64 libz-mingw-w64-dev
#
# packeges to install for the android-sdk
# cmdline-tools
# platform-tools
# built-tools;35.0.1
# ndk;23.2.8568313
# ndk;28.2.13676358

# ==== SETUP ====

TOOLS_DIR="/tmp/gde_gozen_tools"

mkdir -p "$TOOLS_DIR"
if [[ ":$PATH:" != *":$TOOLS_DIR:"* ]]; then
    export PATH="$TOOLS_DIR:$PATH"
fi

# Windows x86_64
ln -sf $(which x86_64-w64-mingw32-gcc-posix) "$TOOLS_DIR/x86_64-w64-mingw32-gcc"
ln -sf $(which x86_64-w64-mingw32-g++-posix) "$TOOLS_DIR/x86_64-w64-mingw32-g++"

# Windows x86_32
ln -sf $(which i686-w64-mingw32-gcc-posix) "$TOOLS_DIR/i686-w64-mingw32-gcc"
ln -sf $(which i686-w64-mingw32-g++-posix) "$TOOLS_DIR/i686-w64-mingw32-g++"

# Linux x86_64
ln -sf $(which pkg-config) "$TOOLS_DIR/pkg-config"

# Linux arm64
ln -sf $(which pkg-config) "$TOOLS_DIR/aarch64-linux-gnu-pkg-config"

ln -sf $(which aarch64-linux-gnu-gcc-14) "$TOOLS_DIR/aarch64-linux-gnu-gcc"
ln -sf $(which aarch64-linux-gnu-g++-14) "$TOOLS_DIR/aarch64-linux-gnu-g++"

# Android
export ANDROID_HOME="$HOME/.android-sdk"
export ANDROID_NDK_ROOT="$HOME/.android-sdk/ndk/28.2.13676358"

# ==== BUILD ====

# Linux x86_64 Debug
printf "1\n1\n1\n1\n1\n" | python3 build.py

# Linux x86_64 Release
printf "1\n1\n2\n2\n2\n" | python3 build.py

# Linux arm64 Debug
printf "1\n2\n1\n1\n1\n" | python3 build.py

# Linux arm64 Release
printf "1\n2\n2\n2\n2\n" | python3 build.py


# Windows x86_64 Debug
printf "2\n1\n1\n1\n1\n" | python3 build.py

# Windows x86_64 Release
printf "2\n1\n2\n2\n2\n" | python3 build.py

# Windows x86_32 Debug
printf "2\n2\n1\n1\n1\n" | python3 build.py

# Windows x86_32 Release
printf "2\n2\n2\n2\n2\n" | python3 build.py


# Android arm64 Debug
printf "4\n1\n1\n1\n1\n" | python3 build.py

# Android arm64 Release
printf "4\n1\n2\n2\n2\n" | python3 build.py

# Android armv7a Debug
printf "4\n2\n1\n1\n1\n" | python3 build.py

# Android armv7a Release
printf "4\n2\n2\n2\n2\n" | python3 build.py

# ==== CLEAN ====

scons -c
rm -rf "$TOOLS_DIR"
