#!/bin/bash

# MySQL Server Start Script
# Usage: export rcc_data_dir=/path/to/data rcc_log_dir=/path/to/log && ./run_server.sh

# ==========================================
# PATH CONFIGURATION
# ==========================================
BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ==========================================
# DEFAULT CONFIGURATION
# ==========================================
PAGE_SIZE=4
BUILD_MODE="rcc"

# ==========================================
# HELPER FUNCTIONS
# ==========================================
print_header() {
    echo ""
    echo "=========================================="
    echo "$1"
    echo "=========================================="
}

print_info() {
    echo "[INFO] $1"
}

print_error() {
    echo "[ERROR] $1"
}

usage() {
    echo "Usage: export rcc_data_dir=/path/to/data rcc_log_dir=/path/to/log && $0 [options]"
    echo ""
    echo "Required environment variables:"
    echo "  rcc_data_dir          PostgreSQL data directory"
    echo "  rcc_log_dir           PostgreSQL log directory"
    echo ""
    echo "Options:"
    echo "  --vanilla             Build vanilla MySQL (without RCC)"
    echo "  --rcc                 Build RCC on MySQL"
    echo "  --bamboo              Build Bamboo on MySQL"
    echo "  --skip-build          Skip build step (use existing binaries)"
    echo "  -h, --help            Show this help"
    exit 0
}

# ==========================================
# PARSE ARGUMENTS
# ==========================================
DATA_DIR="${rcc_data_dir:-}"
LOG_DIR="${rcc_log_dir:-}"
SKIP_BUILD=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --vanilla)
            # Stop existing
            bash shutdown.sh --vanilla
            BUILD_MODE="vanilla"
            shift
            ;;
        --rcc)
            # Stop existing
            bash shutdown.sh --rcc
            BUILD_MODE="rcc"
            shift
            ;;
        --bamboo)
            # Stop existing
            bash shutdown.sh --bamboo
            BUILD_MODE="bamboo"
            shift
            ;;
        --skip-build)
            SKIP_BUILD=true
            shift
            ;;
        -h|--help)
            usage
            ;;
        *)
            print_error "Unknown option: $1"
            usage
            ;;
    esac
done

# Validate required env vars
if [[ -z "$DATA_DIR" ]]; then
    print_error "rcc_data_dir is required"
    usage
fi

if [[ -z "$LOG_DIR" ]]; then
    print_error "rcc_log_dir is required"
    usage
fi

# Get sudo password (from env or prompt)
if [[ -z "$PASSWD" ]]; then
    read -s -p "Enter sudo password: " PASSWD
    echo ""
fi

# ==========================================
# MAIN EXECUTION
# ==========================================
print_header "MySQL Build and Start"


MYSQL_HOME="${BASE_DIR}/${BUILD_MODE}/mysql-8.4.5"
BLD_PATH="$MYSQL_HOME-build"
INSTALL_PATH="$MYSQL_HOME-install"
MYSQL_BIN="${BLD_PATH}/bin/mysqld"

# Build MySQL
if [[ "$SKIP_BUILD" == false ]]; then
    print_info "Building MySQL ($BUILD_MODE mode)..."
    mkdir -p $BLD_PATH
    cd $BLD_PATH
    echo $PASSWD | sudo -S rm -rf $BLD_PATH/*

    cmake $MYSQL_HOME \
        -DCMAKE_C_COMPILER=/usr/bin/gcc \
        -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
        -DDOWNLOAD_BOOST=ON -DWITH_BOOST=$MYSQL_HOME/boost \
        -DWITH_AUTHENTICATION_PLUGIN=mysql_native_password \
        -DWITH_PLUGIN_AUTH_NATIVE=DYNAMIC \
        -DCMAKE_INSTALL_PREFIX=$INSTALL_PATH

    if [ $? -ne 0 ]; then
        print_error "Build failed."
        exit 1
    fi

    make -j$(nproc) install 
    if [ $? -eq 0 ];
    then
        print_info "MAKE SUCCESS"
    else
        print_info "MAKE FAIL"
        exit 1
    fi
else
    print_info "Skipping build (using existing binaries)..."
fi

# Initialize database (only if DATA_DIR is empty)
if [ -z "$(ls -A $DATA_DIR 2>/dev/null)" ]; then
    print_info "Initializing database..."
    cd $BASE_DIR
    
    if grep -Eq '^[[:space:]]*pid-file[[:space:]]*=' "$BASE_DIR/my.cnf"; then
        sed -i -E "s|^[[:space:]]*pid-file[[:space:]]*=.*$|pid-file=${DATA_DIR}/mysql.pid|" "$BASE_DIR/my.cnf"
    if grep -Eq '^[[:space:]]*datadir[[:space:]]*=' "$BASE_DIR/my.cnf"; then
        sed -i -E "s|^[[:space:]]*datadir[[:space:]]*=.*$|datadir=${DATA_DIR}|" "$BASE_DIR/my.cnf"
    if grep -Eq '^[[:space:]]*log-error[[:space:]]*=' "$BASE_DIR/my.cnf"; then
        sed -i -E "s|^[[:space:]]*log-error[[:space:]]*=.*$|log-error=${LOG_DIR}/mysql_error.log|" "$BASE_DIR/my.cnf"
    if grep -Eq '^[[:space:]]*innodb_log_group_home_dir[[:space:]]*=' "$BASE_DIR/my.cnf"; then
        sed -i -E "s|^[[:space:]]*innodb_log_group_home_dir[[:space:]]*=.*$|innodb_log_group_home_dir=${LOG_DIR}|" "$BASE_DIR/my.cnf"
    
    bash $BASE_DIR/scripts/init_database.sh $BUILD_MODE

else
    print_info "Using existing data directory..."
fi

# Start MySQL
print_info "Starting MySQL..."
$MYSQL_BIN --defaults-file=$BASE_DIR/my.cnf --datadir=$data_dir --disable-log-bin  > /dev/null 2>&1 &
sleep 20


print_header "PostgreSQL Ready"
echo "  Build Mode:  $BUILD_MODE"
echo "  Data Dir:    $DATA_DIR"
echo "  Log Dir:     $LOG_DIR"
echo "  Page Size:   ${PAGE_SIZE}KB"
echo ""
echo "To stop: bash shutdown.sh --$BUILD_MODE"
echo "To connect: $BLD_PATH/bin/mysql -uroot"
