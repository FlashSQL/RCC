W=$1

BASE_DIR=`pwd -P`

data_dir="$BASE_DIR/data_dir"
log_dir="$BASE_DIR/log_dir"

mkdir $data_dir
mkdir $log_dir
rm -rf $data_dir/*
rm -rf $log_dir/*

echo "INIT MYSQL"
$BASE_DIR/mysql-8.4.5-build/bin/mysqld --defaults-file=$BASE_DIR/my.cnf --initialize-insecure --user=mysql --datadir=$data_dir --basedir=$BASE_DIR/mysql-8.4.5
sleep 10

echo "RUN MYSQL"
$BASE_DIR/mysql-8.4.5-build/bin/mysqld --defaults-file=$BASE_DIR/my.cnf --datadir=$data_dir & >/dev/null
sleep 30

echo "SET PASSWORD on MYSQL"
$BASE_DIR/mysql-8.4.5-build/bin/mysql -uroot mysql -e "DROP USER IF EXISTS 'root'@'%';"
$BASE_DIR/mysql-8.4.5-build/bin/mysql -uroot mysql -e "CREATE USER 'root'@'%' IDENTIFIED BY '';"
$BASE_DIR/mysql-8.4.5-build/bin/mysql -uroot mysql -e "GRANT ALL PRIVILEGES ON *.* TO 'root'@'%' WITH GRANT OPTION;"
$BASE_DIR/mysql-8.4.5-build/bin/mysql -uroot mysql -e "FLUSH PRIVILEGES;"
$BASE_DIR/mysql-8.4.5-build/bin/mysqladmin -uroot shutdown
sleep 5

echo "RUN MYSQL"
$BASE_DIR/mysql-8.4.5-build/bin/mysqld --defaults-file=$BASE_DIR/my.cnf --datadir=$data_dir --disable-log-bin & >/dev/null
sleep 20

echo "CREATE TPCC"
$BASE_DIR/mysql-8.4.5-build/bin/mysql -u root  tpcc < $BASE_DIR/synthetic_benchmark/create_table_synthetic.sql

echo "SHUTDOWN"
$BASE_DIR/mysql-8.4.5-build/bin/mysqladmin -uroot shutdown
sleep 5