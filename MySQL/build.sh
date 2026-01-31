BASE_DIR=`pwd -P`

# Build
# Result: build directory(mysql-8.4.5-build) and install directory(mysql-8.4.5-install)
mkdir mysql-8.4.5-build
cd $BASE_DIR/mysql-8.4.5-build
cmake ../mysql-8.4.5 \
  -DCMAKE_C_COMPILER=/usr/bin/gcc \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DDOWNLOAD_BOOST=ON -DWITH_BOOST=$BASE_DIR/mysql-8.4.5/boost \
  -DWITH_AUTHENTICATION_PLUGIN=mysql_native_password \
  -DWITH_PLUGIN_AUTH_NATIVE=DYNAMIC \
  -DCMAKE_INSTALL_PREFIX=$BASE_DIR/mysql-8.4.5-install

make -j$(nproc) install 

