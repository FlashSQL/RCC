#!/bin/bash

BASE_DIR=`pwd -P`

data_dir="$BASE_DIR/data_dir"
log_dir="$BASE_DIR/log_dir"

cd $BASE_DIR/mysql-8.4.5-build
make -j$(nproc) install 

if [ $? -eq 0 ];
then
    echo "MAKE SUCCESS"
    
    $BASE_DIR/mysql-8.4.5-build/bin/mysqld \
        --defaults-file=$BASE_DIR/my.cnf \
        --datadir=$data_dir \
        --disable-log-bin  > /dev/null 2>&1 &
    echo "RUNNING..."
    sleep 20
    echo "SERVER START"
else
    echo "MAKE FAIL"
    exit 1
fi