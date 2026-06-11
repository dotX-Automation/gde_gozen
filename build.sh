#!/bin/bash

# ==== SETUP ====

# Windows
sudo update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-posix
sudo update-alternatives --set x86_64-w64-mingw32-g++ /usr/bin/x86_64-w64-mingw32-g++-posix

sudo update-alternatives --set i686-w64-mingw32-gcc /usr/bin/i686-w64-mingw32-gcc-posix
sudo update-alternatives --set i686-w64-mingw32-g++ /usr/bin/i686-w64-mingw32-g++-posix

# Linux
mkdir -p /tmp/ffmpeg-build-tools                                                           
ln -s $(which pkg-config) /tmp/ffmpeg-build-tools/aarch64-linux-gnu-pkg-config
if [[ ":$PATH:" != *":/tmp/ffmpeg-build-tools:"* ]]; then
    export PATH="/tmp/ffmpeg-build-tools:$PATH"
fi

# Android
export ANDROID_HOME="$HOME/.android-sdk"
export ANDROID_NDK_ROOT="$HOME/.android-sdk/ndk/28.2.13676358"

# ===============

scons -c

# Linux x86_64 Debug
printf "0\n1\n1\n1\n1\n1\n" | python3 build.py

# Linux x86_64 Release
printf "1\n1\n1\n2\n1\n2\n" | python3 build.py

# Linux arm64 Debug
printf "1\n1\n2\n1\n1\n1\n" | python3 build.py

# Linux arm64 Release
printf "1\n1\n2\n2\n1\n2\n" | python3 build.py


# Windows x86_64 Debug
printf "1\n2\n1\n1\n1\n1\n" | python3 build.py

# Windows x86_64 Release
printf "1\n2\n1\n2\n1\n2\n" | python3 build.py

# Windows x86_32 Debug
printf "1\n2\n2\n1\n1\n1\n" | python3 build.py

# Windows x86_32 Release
printf "1\n2\n2\n2\n1\n2\n" | python3 build.py


# Android arm64 Debug
printf "1\n4\n1\n1\n1\n1\n" | python3 build.py

# Android arm64 Release
printf "1\n4\n1\n2\n1\n2\n" | python3 build.py

# Android armv7a Debug
printf "1\n4\n2\n1\n1\n1\n" | python3 build.py

# Android armv7a Release
printf "1\n4\n2\n2\n1\n2\n" | python3 build.py
