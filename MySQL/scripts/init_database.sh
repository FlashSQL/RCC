BASE_DIR=`pwd -P`

data_dir="${rcc_data_dir:-}"
BUILD_MODE=$1

MYSQL_HOME="${BASE_DIR}/${BUILD_MODE}/mysql-8.4.5"
BLD_PATH="$MYSQL_HOME-build"
MYSQL_BIN="${BLD_PATH}/bin/mysqld"
MYSQL="${BLD_PATH}/bin/mysqld"

rm -rf $data_dir/*

echo "INIT MYSQL"
$MYSQL_BIN --defaults-file=$BASE_DIR/my.cnf --initialize-insecure --user=mysql --datadir=$data_dir --basedir=$MYSQL_HOME
sleep 10

echo "RUN MYSQL"
$MYSQL_BIN --defaults-file=$BASE_DIR/my.cnf --datadir=$data_dir & >/dev/null
sleep 30

echo "SET PASSWORD on MYSQL"
$MYSQL -uroot mysql -e "DROP USER IF EXISTS 'root'@'%';"
$MYSQL -uroot mysql -e "CREATE USER 'root'@'%' IDENTIFIED BY '';"
$MYSQL -uroot mysql -e "GRANT ALL PRIVILEGES ON *.* TO 'root'@'%' WITH GRANT OPTION;"
$MYSQL -uroot mysql -e "FLUSH PRIVILEGES;"
$MYSQLadmin -uroot shutdown
sleep 5

echo "RUN MYSQL"
$MYSQLd --defaults-file=$BASE_DIR/my.cnf --datadir=$data_dir --disable-log-bin & >/dev/null
sleep 20