#!/bin/bash
# Shared settings and helpers of the DBx1000 experiment scripts (sourced, not run).
# Every run copies a config of configs/ with the algorithm and the thread count of the run into config.h, builds rundb
# and runs it for 30 s under numactl --interleave all. The paper runs threads 1..128, 3 runs per point.
# Needs g++, make, numactl, lscpu and TBB (system libtbb-dev, or a oneTBB build in ../tbb).
# CPU_FREQ in the configs is the TSC rate of the 128-core server (2.25 GHz): set it to the TSC rate of another machine.

# ==========================================
# PATH CONFIGURATION
# ==========================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CONFIG_DIR="$BASE_DIR/configs"

# ==========================================
# CONFIGURATION (paper settings)
# ==========================================
PAPER_THREADS="1,16,32,64,96,128"
PAPER_REPEAT=3
RUN_TIMEOUT=1800                                  # seconds per run
JOBS=$(( $(nproc) < 32 ? $(nproc) : 32 ))         # make -j

# Default: a quick pass, every thread count of the paper once. --paper: 3 runs per point, as in the paper.
THREADS="$PAPER_THREADS"
REPEAT=1
DRY_RUN=false

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
    sed -n '2,/^$/s/^# \{0,1\}//p' "$0"   # the header comment of the calling script
    echo "Options:"
    echo "  --threads <list>       Comma-separated thread counts (default: $PAPER_THREADS)"
    echo "  --paper                $PAPER_REPEAT runs per point, as in the paper (default: 1)"
    echo "  --results-dir <path>   Results directory (default: $RESULTS_DIR)"
    echo "  --dry-run              Print the runs and their CPUs; build and run nothing"
    echo "  -h, --help             Show this help"
    exit 0
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            --threads)     THREADS="$2"; shift 2 ;;
            --paper)       REPEAT=$PAPER_REPEAT; shift ;;
            --results-dir) RESULTS_DIR="$2"; shift 2 ;;
            --dry-run)     DRY_RUN=true; shift ;;
            -h|--help)     usage ;;
            *)             print_error "Unknown option: $1"; usage ;;
        esac
    done
}

# ==========================================
# CONFIG FUNCTIONS
# ==========================================
# set_knob <file> <knob> <value>: replace the #define of a knob
set_knob() {
    if ! grep -qE "^#define[[:space:]]+$2\b" "$1"; then
        print_error "$2 is not defined in $1"
        exit 1
    fi
    sed -i -E "s/^#define[[:space:]]+$2\b.*/#define $2 $3/" "$1"
}

# make_config <config> <CC_ALG> <threads> <out>: the config of one run.
# MAX_THREAD, the query pool (MAX_TXN_PER_PART, sized for 30 s runs) and the YCSB table size depend on the thread count.
make_config() {
    local cfg=$1 alg=$2 t=$3 out=$4
    cp "$CONFIG_DIR/$cfg" "$out"
    set_knob "$out" CC_ALG "$alg"
    set_knob "$out" THREAD_CNT "$t"
    set_knob "$out" MAX_THREAD $(( t > 96 ? t : 96 ))
    local pool=$(( 30000000 / t ))
    if (( pool > 7000000 )); then pool=7000000; fi
    if (( pool < 100000 )); then pool=100000; fi
    set_knob "$out" MAX_TXN_PER_PART "$pool"
    if grep -qE '^#define[[:space:]]+WORKLOAD[[:space:]]+YCSB\b' "$out"; then
        # the loader drops remainder keys: the table size must be a multiple of THREAD_CNT and of 40 (INIT_PARALLELISM)
        local a=$t b=40 r
        while (( b > 0 )); do r=$(( a % b )); a=$b; b=$r; done
        local step=$(( t * 40 / a ))
        set_knob "$out" SYNTH_TABLE_SIZE $(( YCSB_ROWS - YCSB_ROWS % step ))
    fi
}

# ==========================================
# CPU FUNCTIONS
# ==========================================
# read_topology: CPUS = worker order (per socket, one hardware thread of every core, then the SMT siblings),
# PHYS = one hardware thread per physical core in that order, CORE_OF = cpu -> socket:core
read_topology() {
    local cpu core sock
    local -A first=()
    declare -gA CORE_OF=()
    while IFS=, read -r cpu core sock; do
        CORE_OF[$cpu]="$sock:$core"
        if [[ -z ${first[$sock:$core]} ]]; then first[$sock:$core]=$cpu; fi
    done < <(lscpu -p=CPU,CORE,SOCKET | grep -v '^#' | sort -t, -k3,3n -k1,1n)
    CPUS=($(lscpu -p=CPU,CORE,SOCKET | grep -v '^#' | sort -t, -k3,3n -k1,1n | awk -F, '
        $3 != s { printf "%s%s", head, tail; head = tail = ""; s = $3 }
        { if (($3 ":" $2) in seen) tail = tail " " $1; else { head = head " " $1; seen[$3 ":" $2] = 1 } }
        END { print head tail }'))
    PHYS=()
    for cpu in "${CPUS[@]}"; do
        if [[ ${first[${CORE_OF[$cpu]}]} == "$cpu" ]]; then PHYS+=("$cpu"); fi
    done
}

# place_cpus <threads> <rcc: true|false> <detector threads>: sets CPU_MAP (worker i -> CPU), UNPINNED_CPUS (the CPUs
# the unpinned workers may use) and DETECT_CPUS. The RCC detectors get CPUs whose whole physical core is kept free of
# workers: the CPUs right after the workers, or the last physical cores when workers + detectors exceed the physical cores.
place_cpus() {
    local t=$1 rcc=$2 nd=$3 i c n
    local -a workers=("${CPUS[@]}") dets=() free=() map=()
    local -A busy=()
    if [[ $rcc == true ]]; then
        if (( t + nd > ${#PHYS[@]} )); then
            for (( i = 1; i <= nd; i++ )); do dets+=("${PHYS[${#PHYS[@]} - i]}"); done
        else
            for (( i = 0; i < nd; i++ )); do dets+=("${CPUS[(t + i) % ${#CPUS[@]}]}"); done
        fi
        for c in "${dets[@]}"; do busy[${CORE_OF[$c]}]=1; done
        for c in "${CPUS[@]}"; do
            if [[ -z ${busy[${CORE_OF[$c]}]} ]]; then free+=("$c"); fi
        done
        if (( ${#free[@]} >= t )); then workers=("${free[@]}"); else dets=(); fi   # too few CPUs: shared
    fi
    n=${#workers[@]}
    for (( i = 0; i < t; i++ )); do map+=("${workers[i % n]}"); done
    CPU_MAP=$(IFS=,; echo "${map[*]}")
    UNPINNED_CPUS=$(IFS=,; echo "${workers[*]}")
    DETECT_CPUS=""
    if [[ $rcc == true ]]; then
        if (( ${#dets[@]} == 0 )); then
            for (( i = 0; i < nd; i++ )); do dets+=("${workers[(t + i) % n]}"); done
        fi
        DETECT_CPUS=$(IFS=,; echo "${dets[*]}")
    fi
}

# ==========================================
# RUN FUNCTIONS
# ==========================================
# run_point <name> <config> <CC_ALG> <threads> <rep>
run_point() {
    local name=$1 cfg=$2 alg=$3 t=$4 rep=$5
    local dir="$RESULTS_DIR/${name}_${t}_r${rep}"
    local nd
    nd=$(awk '$1 == "#define" && $2 == "RCC_ADD_THREADS" { print $3 }' "$CONFIG_DIR/$cfg")
    place_cpus "$t" "$([[ $alg == RCC ]] && echo true || echo false)" "${nd:-1}"
    print_info "$name threads=$t r$rep  workers: $CPU_MAP  detector: ${DETECT_CPUS:-none}"
    if [[ $DRY_RUN == true ]]; then return 0; fi

    mkdir -p "$dir"
    make_config "$cfg" "$alg" "$t" "$dir/config.h"
    cp "$dir/config.h" "$BASE_DIR/config.h"
    if ! (cd "$BASE_DIR" && make clean > /dev/null 2>&1 && make -j"$JOBS" > "$dir/build.log" 2>&1); then
        print_error "Build failed (TBB installed?): $dir/build.log"
        tail -5 "$dir/build.log"
        return 1
    fi

    local env_args=(CPU_MAP="$CPU_MAP" UNPINNED_CPUS="$UNPINNED_CPUS")
    if [[ -n $DETECT_CPUS ]]; then env_args+=(RCC_DETECT_CPUS="$DETECT_CPUS"); fi
    printf '%s\n' "${env_args[@]}" "numactl --interleave all" > "$dir/run.env"
    (cd "$BASE_DIR" && timeout "$RUN_TIMEOUT" env -u RCC_DETECT_CPU -u RCC_DETECT_CPUS "${env_args[@]}" \
        numactl --interleave all ./rundb) > "$dir/run.out" 2>&1

    local summary tps abort
    summary=$(grep '^\[summary\]' "$dir/run.out" | tail -1)
    if [[ -z $summary ]]; then
        print_error "No [summary] line: $dir/run.out"
        return 1
    fi
    tps=$(grep -oE 'throughput=[^,]+' <<< "$summary" | cut -d= -f2)
    abort=$(grep -oE 'abort_rate=[^,]+' <<< "$summary" | cut -d= -f2)
    printf '%-12s %8s %4s %16s %12s\n' "$name" "$t" "$rep" "$tps" "$abort" >> "$RESULTS_DIR/summary.txt"
    print_info "DONE $name threads=$t r$rep  throughput=$tps  abort_rate=$abort"
}

# run_experiment: every entry of RUNS ("name:config:CC_ALG") for every thread count, REPEAT times
run_experiment() {
    local t entry name cfg alg rep
    print_header "$TITLE"
    print_info "Runs:    ${RUNS[*]}"
    print_info "Threads: $THREADS (x$REPEAT, 30s each)"
    print_info "Results: $RESULTS_DIR"
    read_topology
    if [[ $DRY_RUN == false ]]; then
        mkdir -p "$RESULTS_DIR"
        if [[ ! -f "$RESULTS_DIR/summary.txt" ]]; then
            printf '%-12s %8s %4s %16s %12s\n' name threads rep throughput abort_rate > "$RESULTS_DIR/summary.txt"
        fi
    fi
    for t in ${THREADS//,/ }; do
        for entry in "${RUNS[@]}"; do
            IFS=: read -r name cfg alg <<< "$entry"
            for (( rep = 1; rep <= REPEAT; rep++ )); do
                run_point "$name" "$cfg" "$alg" "$t" "$rep"
            done
        done
    done
}
