#!/bin/sh
# git submodule update --init --recursive

rm build -rf
mkdir -p build && cd build

cmake3 ..
make -j
make install
