#!/bin/bash

BASE_DIR=`pwd -P`

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
        --rcc-w)
            BUILD_MODE="RCC-W"
            shift
            ;;
        --rcc-d)
            BUILD_MODE="RCC-D"
            shift
            ;;
        --rcc-c)
            BUILD_MODE="RCC-C"
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


$BASE_DIR/$BUILD_MODE/mysql-8.4.5-build/bin/mysqladmin -uroot shutdown
