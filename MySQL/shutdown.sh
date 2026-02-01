#!/bin/bash

BASE_DIR=`pwd -P`

while [[ $# -gt 0 ]]; do
    case $1 in
        --vanilla)
            BUILD_MODE="vanilla"
            shift
            ;;
        --rcc)
            BUILD_MODE="rcc"
            shift
            ;;
        --bamboo)
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


$BASE_DIR/$BUILD_MODE/mysql-8.4.5-build/bin/mysqladmin -uroot shutdown