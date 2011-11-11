#!/bin/bash -x

set -e

./configure --enable-static --enable-strip --enable-win32thread --enable-lto --chroma-format=all --bit-depth=8 --disable-lavf
