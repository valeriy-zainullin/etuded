#!/bin/bash

cmake --toolchain=toolchain-mingw-w64.cmake -DCROSS_COMPILING_ON_LINUX=1 "$@"
