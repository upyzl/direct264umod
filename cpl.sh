#!/bin/bash -x

set -e

./configure --enable-static --enable-strip --enable-win32thread --enable-lto --bit-depth=8 --disable-lavf
