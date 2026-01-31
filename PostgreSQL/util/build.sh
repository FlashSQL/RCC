#!/bin/bash

BASE_DIR=`pwd -P`

# Build
# Result: build directory in rcc_postgresql/bld
cd $BASE_DIR/rcc_postgresql
mkdir -p bld
cd bld

../configure --prefix=$BASE_DIR/rcc_postgresql/bld \
  --with-blocksize=16 \
  CFLAGS="-O2 -DSHADOWTX"

make -j$(nproc) install-world
