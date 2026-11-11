#!/bin/bash
# Shared paths and CMake options for the RCC and Bamboo source trees.

mysql_build_config() {
    local mode="$1"
    local base_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    local source_group suffix
    MYSQL_CMAKE_OPTIONS=()
    case "$mode" in
        Vanilla)
            source_group=Vanilla
            suffix=""
            ;;
        RCC)
            source_group=RCC
            suffix=""
            MYSQL_CMAKE_OPTIONS=(-DWITH_RCC_C=OFF)
            ;;
        RCC-C)
            source_group=RCC
            suffix="-C"
            MYSQL_CMAKE_OPTIONS=(-DWITH_RCC_C=ON)
            ;;
        Bamboo)
            source_group=Bamboo
            suffix=""
            MYSQL_CMAKE_OPTIONS=(-DWITH_REBIRTH=OFF)
            ;;
        Rebirth)
            source_group=Bamboo
            suffix="-RR"
            MYSQL_CMAKE_OPTIONS=(-DWITH_REBIRTH=ON)
            ;;
        *)
            echo "[ERROR] Unsupported build mode: $mode (use Vanilla, RCC, RCC-C, Bamboo or Rebirth)" >&2
            return 1
            ;;
    esac
    MYSQL_HOME="$base_dir/$source_group/mysql-8.4.5"
    BLD_PATH="$MYSQL_HOME-build$suffix"
    INSTALL_PATH="$MYSQL_HOME-install$suffix"
    INNOBASE_SOURCE="$base_dir/$source_group/innobase"
}
