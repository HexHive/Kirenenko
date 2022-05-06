#!/bin/bash

set -e

# Set compilation env
export CC="/root/Kirenenko/bin/ko-clang"
export CXX="/root/Kirenenko/bin/ko-clang++"
#export CFLAGS="$CFLAGS -g -O0"
#export CXXFLAGS="$CXXFLAGS -g -O0"
#export LD="/root/bug-severity-AFLplusplus/afl-clang-fast"
#export LIBS="$LIBS /root/lib/build/asan/afl/libasan_afl.a"
#export AFL_USE_ASAN=1

# Build
cd "/root/source_17795"

mkdir -p  "/bugs/build/CVE-2018-17795/jigsaw"
./autogen.sh
./configure --disable-shared --prefix="/bugs/build/CVE-2018-17795/jigsaw"
make -j4
make install

