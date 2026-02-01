BASE_DIR=`pwd -P`

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

BUILD_MODE="vanilla"

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
    echo "  -h, --help            Show this help"
    exit 0
}

DATA_DIR="${rcc_data_dir:-}"
LOG_DIR="${rcc_log_dir:-}"


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

while [[ $# -gt 0 ]]; do
    case $1 in
        --vanilla)
            # Stop existing
            bash shutdown.sh --vanilla
            mkdir -p  vanilla
            cd vanilla

            wget https://dev.mysql.com/get/Downloads/MySQL-5.7/mysql-8.4.5.tar.gz
            tar -zxvf mysql-8.4.5.tar.gz

            BUILD_MODE="vanilla"
            shift
            ;;
        --rcc)
            # Stop existing
            bash shutdown.sh --rcc
            mkdir -p  rcc
            cd rcc

            wget https://dev.mysql.com/get/Downloads/MySQL-5.7/mysql-8.4.5.tar.gz
            tar -zxvf mysql-8.4.5.tar.gz

            rm -r $BASE_DIR/rcc/mysql-8.4.5/storage/innobase/
            cp -r $BASE_DIR/rcc/innobase/ $BASE_DIR/rcc/mysql-8.4.5/storage/innobase/
            BUILD_MODE="rcc"
            shift
            ;;
        --bamboo)
            # Stop existing
            bash shutdown.sh --bamboo
            mkdir -p  bamboo
            cd bamboo

            wget https://dev.mysql.com/get/Downloads/MySQL-5.7/mysql-8.4.5.tar.gz
            tar -zxvf mysql-8.4.5.tar.gz

            rm -r $BASE_DIR/bamboo/mysql-8.4.5/storage/innobase/
            cp -r $BASE_DIR/bamboo/innobase/ $BASE_DIR/bamboo/mysql-8.4.5/storage/innobase/
            BUILD_MODE="bamboo"
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



MYSQL_HOME="${BASE_DIR}/${BUILD_MODE}/mysql-8.4.5"
BLD_PATH="$MYSQL_HOME-build"
INSTALL_PATH="$MYSQL_HOME-install"

mkdir -p $BLD_PATH
cd $BLD_PATH

cmake $MYSQL_HOME \
    -DCMAKE_C_COMPILER=/usr/bin/gcc \
    -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
    -DDOWNLOAD_BOOST=ON -DWITH_BOOST=$MYSQL_HOME/boost \
    -DWITH_AUTHENTICATION_PLUGIN=mysql_native_password \
    -DWITH_PLUGIN_AUTH_NATIVE=DYNAMIC \
    -DCMAKE_INSTALL_PREFIX=$INSTALL_PATH

make -j$(nproc) install 