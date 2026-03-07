#!/bin/bash

# Run this script from HOMER (compute node).
# It builds on both machines, then runs the sweep.
set -euo pipefail
trap 'echo "FAILED at line $LINENO (exit code $?). Check $LOG_FILE for details."' ERR

# ================= CONFIGURATION =================
ARYA="arya"
# Local (non-NFS) directories for machine-specific binaries
LOCAL_DIR="/home-local/gejsi/lethe-build"
ARYA_SERVER="$LOCAL_DIR/server"
HOMER_CLIENT="$LOCAL_DIR/client"

ITERATIONS=5
CACHE_SIZES_MB=(3600 2725 1850 975)  # 100%, 75%, 50%, 25% of WSS
RATIOS=(100 75 50 25)

WORKLOADS=(
    "0 uniform A-uniform"
    "0 zipfian A-zipfian"
)

# Rebalancer modes: "1 rebal-on" and "0 rebal-off"
REBALANCER_MODES=(
    "1 rebal-on"
)
    # "0 rebal-off"

KEYS=60000000
OPS=20000000
THREADS=1

DATE=$(date +%Y%m%d_%H%M%S)
OUT_DIR="results_lethe_${DATE}"
SUMMARY_FILE="${OUT_DIR}/summary.csv"
# =================================================

echo "=================================================="
echo "Building on both machines..."
echo "=================================================="

# Build VoliMem + Lethe on ARYA, copy server binary to local path
ssh "$ARYA" "
    cd ~/desk/voli && VOLIMEM_RELEASE=1 make -B -j &&
    cd ~/desk/lethe/build && cmake -DCMAKE_BUILD_TYPE=Release -G Ninja .. && ninja &&
    mkdir -p $LOCAL_DIR &&
    cp bin/server $ARYA_SERVER
" || { echo "ARYA build failed!"; exit 1; }
echo "Arya: done"

# Build VoliMem + Lethe on HOMER, copy client binary to local path
cd ~/desk/voli && VOLIMEM_RELEASE=1 make -B -j
cd ~/desk/lethe/build && cmake -DCMAKE_BUILD_TYPE=Release -G Ninja .. && ninja
mkdir -p "$LOCAL_DIR"
cp bin/client "$HOMER_CLIENT"
echo "Homer: done"

echo "=================================================="
echo "Starting Lethe Sweep"
echo "Iterations: ${ITERATIONS}"
echo "Output: $OUT_DIR"
echo "=================================================="

mkdir -p "$OUT_DIR"
echo "Workload,Rebalancer,Ratio,Iteration,Cache_MB,Load_Throughput,Work_Throughput" > "$SUMMARY_FILE"

for WL_ENTRY in "${WORKLOADS[@]}"; do
    read -r WL_ID WL_DIST WL_LABEL <<< "$WL_ENTRY"

    echo ""
    echo "================== Workload: $WL_LABEL =================="

    for RB_ENTRY in "${REBALANCER_MODES[@]}"; do
        read -r RB_FLAG RB_LABEL <<< "$RB_ENTRY"

        echo ""
        echo "============== Rebalancer: $RB_LABEL =============="

        for idx in "${!RATIOS[@]}"; do
            RATIO=${RATIOS[$idx]}
            CACHE_MB=${CACHE_SIZES_MB[$idx]}

            echo ""
            echo "--------------------------------------------------"
            echo "[$WL_LABEL | $RB_LABEL] ${RATIO}% Local Memory (cache: ${CACHE_MB} MB)"
            echo "--------------------------------------------------"

            for ((i=1; i<=ITERATIONS; i++)); do
                LOG_FILE="${OUT_DIR}/${WL_LABEL}_${RB_LABEL}_ratio_${RATIO}_run_${i}.log"
                echo -n "[$WL_LABEL $RB_LABEL ${RATIO}%] Run $i/$ITERATIONS... "

                # 1. Start server on arya from local binary
                ssh "$ARYA" "pkill -f '$ARYA_SERVER' 2>/dev/null; true" || true
                sleep 0.5
                ssh "$ARYA" "nohup $ARYA_SERVER > /tmp/lethe-server.log 2>&1 & sleep 0.5 && if ! kill -0 \$! 2>/dev/null; then echo 'Server died immediately:'; cat /tmp/lethe-server.log; exit 1; fi"
                sleep 2

                # 2. Run client on homer from local binary
                if ! $HOMER_CLIENT \
                    -a 10.0.0.2 \
                    -m "$CACHE_MB" \
                    -r "$RB_FLAG" \
                    -t "$THREADS" \
                    -k "$KEYS" \
                    -o "$OPS" \
                    -d "$WL_DIST" \
                    -w "$WL_ID" \
                    > "$LOG_FILE" 2>&1; then
                    echo "FAILED! Client log:"
                    cat "$LOG_FILE"
                    echo "Server log:"
                    ssh "$ARYA" "cat /tmp/lethe-server.log 2>/dev/null" || true
                    exit 1
                fi

                # 3. Kill server on arya
                ssh "$ARYA" "pkill -f '$ARYA_SERVER' 2>/dev/null" || true

                # 4. Extract metrics
                LOAD_TP=$(grep "load:" "$LOG_FILE" | awk '{print $2}')
                WORK_TP=$(grep "work:" "$LOG_FILE" | awk '{print $2}')

                echo "Done. Load: $LOAD_TP | Work: $WORK_TP ops/s"

                # 5. Append to CSV
                echo "${WL_LABEL},${RB_LABEL},${RATIO},${i},${CACHE_MB},${LOAD_TP},${WORK_TP}" >> "$SUMMARY_FILE"

                sleep 2
            done
        done
    done
done

echo ""
echo "=================================================="
echo "Sweep Complete."
echo "Summary saved to: $SUMMARY_FILE"
echo "=================================================="
