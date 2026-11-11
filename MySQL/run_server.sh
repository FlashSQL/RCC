#!/bin/bash
set -e

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
BUILD_MODE="RCC"
source "$BASE_DIR/scripts/build_config.sh"

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
    echo "  rcc_data_dir          MySQL data directory"
    echo "  rcc_log_dir           MySQL log directory"
    echo ""
    echo "Options:"
    echo "  --vanilla             Build vanilla MySQL (without RCC)"
    echo "  --rcc                 Build RCC on MySQL"
    echo "  --rcc-c               Build RCC-C on MySQL"
    echo "  --bamboo              Build Bamboo (WITH_REBIRTH=OFF)"
    echo "  --rebirth             Build Rebirth (WITH_REBIRTH=ON)"
    echo "  --build-only          Build and install without starting a server (no data/log env needed)"
    echo "  --skip-build          Skip build step (use existing binaries)"
    echo "  -h, --help            Show this help"
    exit "${1:-0}"
}

# ==========================================
# PARSE ARGUMENTS
# ==========================================
DATA_DIR="${rcc_data_dir:-}"
LOG_DIR="${rcc_log_dir:-}"
SKIP_BUILD=false
BUILD_ONLY=false
while [[ $# -gt 0 ]]; do
    case $1 in
        --vanilla)
            BUILD_MODE="Vanilla"
            shift
            ;;
        --rcc)
            BUILD_MODE="RCC"
            shift
            ;;
        --rcc-c)
            BUILD_MODE="RCC-C"
            shift
            ;;
        --bamboo)
            BUILD_MODE="Bamboo"
            shift
            ;;
        --rebirth)
            BUILD_MODE="Rebirth"
            shift
            ;;
        --build-only)
            BUILD_ONLY=true
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
            usage 1
            ;;
    esac
done

if [[ "$BUILD_ONLY" == true && "$SKIP_BUILD" == true ]]; then
    print_error "--build-only and --skip-build cannot be combined"
    exit 1
fi

# Building alone does not require or touch a database.
if [[ "$BUILD_ONLY" == false ]]; then
    if [[ -z "$DATA_DIR" || -z "$LOG_DIR" ]]; then
        print_error "rcc_data_dir and rcc_log_dir are required"
        usage 1
    fi
fi

# ==========================================
# MAIN EXECUTION
# ==========================================
print_header "MySQL Build and Start"


mysql_build_config "$BUILD_MODE"
MYSQL_BIN="${INSTALL_PATH}/bin/mysqld"

if [[ "$SKIP_BUILD" == false ]]; then
    print_info "Building MySQL ($BUILD_MODE mode)..."

    if [[ ! -d "$MYSQL_HOME" ]]; then
        SOURCE_PARENT="$(dirname "$MYSQL_HOME")"
        mkdir -p "$SOURCE_PARENT"
        ARCHIVE="$SOURCE_PARENT/mysql-8.4.5.tar.gz"
        wget -O "$ARCHIVE" https://downloads.mysql.com/archives/get/p/23/file/mysql-8.4.5.tar.gz
        tar -xzf "$ARCHIVE" -C "$SOURCE_PARENT"
        rm -f "$ARCHIVE"
    fi

    # Refresh the overlay on every build, including an existing downloaded tree.
    # RCC-C shares RCC's source; Rebirth shares Bamboo's source.
    # Each variant has its own CMake flag and output paths.
    if [[ "$BUILD_MODE" != "Vanilla" ]]; then
        rm -rf "$MYSQL_HOME/storage/innobase"
        cp -a "$INNOBASE_SOURCE" "$MYSQL_HOME/storage/innobase"
    fi

    cmake -S "$MYSQL_HOME" -B "$BLD_PATH" \
        -DCMAKE_C_COMPILER="${CC:-gcc-10}" \
        -DCMAKE_CXX_COMPILER="${CXX:-g++-10}" \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_PATH" \
        "${MYSQL_CMAKE_OPTIONS[@]}"
    cmake --build "$BLD_PATH" --parallel "${BUILD_JOBS:-$(nproc)}"
    cmake --install "$BLD_PATH"
else
    print_info "Skipping build (using existing binaries)..."
fi

if [[ "$BUILD_ONLY" == true ]]; then
    print_info "Build complete: $MYSQL_BIN"
    exit 0
fi

if [[ ! -x "$MYSQL_BIN" ]]; then
    print_error "mysqld not found at $MYSQL_BIN"
    exit 1
fi

# Preserve the experiment launcher's fresh-database behavior only on startup.
pkill -9 -ef mysql || true
mkdir -p "$DATA_DIR" "$LOG_DIR"
rm -rf "$DATA_DIR"/*
rm -rf "$LOG_DIR"/*

if [ -z "$(ls -A $DATA_DIR 2>/dev/null)" ]; then
    print_info "Initializing database..."
    cd $BASE_DIR

    if grep -Eq '^[[:space:]]*pid-file[[:space:]]*=' "$BASE_DIR/my.cnf"; then
        sed -i -E "s|^[[:space:]]*pid-file[[:space:]]*=.*$|pid-file=${DATA_DIR}/mysql.pid|" "$BASE_DIR/my.cnf"
    fi
    if grep -Eq '^[[:space:]]*datadir[[:space:]]*=' "$BASE_DIR/my.cnf"; then
        sed -i -E "s|^[[:space:]]*datadir[[:space:]]*=.*$|datadir=${DATA_DIR}|" "$BASE_DIR/my.cnf"
    fi
    if grep -Eq '^[[:space:]]*log-error[[:space:]]*=' "$BASE_DIR/my.cnf"; then
        sed -i -E "s|^[[:space:]]*log-error[[:space:]]*=.*$|log-error=${LOG_DIR}/mysql_error.log|" "$BASE_DIR/my.cnf"
    fi
    if grep -Eq '^[[:space:]]*innodb_log_group_home_dir[[:space:]]*=' "$BASE_DIR/my.cnf"; then
        sed -i -E "s|^[[:space:]]*innodb_log_group_home_dir[[:space:]]*=.*$|innodb_log_group_home_dir=${LOG_DIR}|" "$BASE_DIR/my.cnf"
    fi

    bash $BASE_DIR/scripts/init_database.sh $BUILD_MODE

else
    print_info "Using existing data directory..."
fi

# Start MySQL
print_info "Starting MySQL..."
$MYSQL_BIN --defaults-file=$BASE_DIR/my.cnf --datadir=$DATA_DIR --disable-log-bin  > /dev/null 2>&1 &
sleep 20


print_header "MySQL Ready"
echo "  Build Mode:  $BUILD_MODE"
echo "  Data Dir:    $DATA_DIR"
echo "  Log Dir:     $LOG_DIR"
echo "  Page Size:   ${PAGE_SIZE}KB"
echo ""
echo "To stop: bash shutdown.sh --$(echo $BUILD_MODE | tr '[:upper:]' '[:lower:]')"
echo "To connect: $INSTALL_PATH/bin/mysql -uroot"
