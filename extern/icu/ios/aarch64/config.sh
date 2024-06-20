#!/bin/sh

. ../crossenv.sh

../source/configure --prefix=$(pwd)/prebuilt \
    --host=aarch64-apple-darwin \
    --enable-static=no \
    --enable-shared \
    --enable-extras=no \
    --enable-strict=no \
    --enable-icuio=no \
    --enable-layout=no \
    --enable-layoutex=no \
    --enable-tools=no \
    --enable-tests=no \
    --enable-samples=no \
    --enable-renaming \
    --enable-dyload \
    --with-cross-build=`realpath ../linux` \
    CFLAGS='-Os' \
    CXXFLAGS='--std=c++11' \
    LDFLAGS='-static-libstdc++' \
    --with-data-packaging=archive
