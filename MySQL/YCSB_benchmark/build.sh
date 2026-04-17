#!/bin/bash
# Build the YCSB JDBC binding (requires Maven + JDK).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

mvn -B -pl site.ycsb:jdbc-binding -am package -DskipTests
