#!/bin/bash

# ================= CONFIGURATION =================
# Working Set Size in MB (60M keys ~ 4GB)
WSS_MB=3500

# Buffer for Kernel Memory overhead (Stack, Page Tables, Slab)
# Prevents OOM-Killer from shooting the app immediately.
OVERHEAD_MB=100

# Number of times to repeat each test
ITERATIONS=5

# The Ratios to test (Local Memory %)
RATIOS=(100 75 50 25)

# Workloads to test: "ID DIST LABEL"
# 0=A(50R/50W), 1=B(95R/5W), 2=C(100R), 3=D(95R/5I)
WORKLOADS=(
    "0 uniform A-uniform"
    "0 zipfian A-zipfian"
)

# Output Directory
DATE=$(date +%Y%m%d_%H%M%S)
OUT_DIR="results_volimem_${DATE}"
SUMMARY_FILE="${OUT_DIR}/summary.csv"

# Benchmark Command settings
KEYS=60000000
OPS=20000000
THREADS=1
# =================================================

mkdir -p "$OUT_DIR"
echo "Workload,Ratio,Iteration,Limit_MB,Load_Throughput,Work_Throughput,Wall_Time" > "$SUMMARY_FILE"

echo "=================================================="
echo "Starting VoliMem (no swapper) Sweep"
echo "WSS: ${WSS_MB} MB | Iterations: ${ITERATIONS}"
echo "Output: $OUT_DIR"
echo "=================================================="

for WL_ENTRY in "${WORKLOADS[@]}"; do
    read -r WL_ID WL_DIST WL_LABEL <<< "$WL_ENTRY"

    echo ""
    echo "================== Workload: $WL_LABEL =================="

    for RATIO in "${RATIOS[@]}"; do

        # Calculate Memory Limit for this Ratio
        # Formula: (WSS * Ratio / 100) + Overhead
        TARGET_MEM=$(( (WSS_MB * RATIO) / 100 ))
        LIMIT_MB=$(( TARGET_MEM + OVERHEAD_MB ))

        echo ""
        echo "--------------------------------------------------"
        echo "[$WL_LABEL] ${RATIO}% Local Memory"
        echo "Target App Mem: ${TARGET_MEM} MB"
        echo "Cgroup Limit:   ${LIMIT_MB} MB (incl. buffer)"
        echo "--------------------------------------------------"

        for ((i=1; i<=ITERATIONS; i++)); do
            LOG_FILE="${OUT_DIR}/${WL_LABEL}_ratio_${RATIO}_run_${i}.log"
            echo -n "[$WL_LABEL ${RATIO}%] Run $i/$ITERATIONS... "

            # 1. Clear Page Cache / Dentries to ensure Cold Start
            sync; echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null

            # 2. Run Benchmark inside Systemd Scope
            sudo systemd-run --scope \
                -p MemoryMax=${LIMIT_MB}M \
                -p MemorySwapMax=infinity \
                /usr/bin/time -v \
                bin/run_benchmark_volimem -t $THREADS -k $KEYS -o $OPS -d $WL_DIST -w $WL_ID \
                > "$LOG_FILE" 2>&1

            # 3. Extract Metrics
            LOAD_TP=$(grep "load:" "$LOG_FILE" | awk '{print $2}')
            WORK_TP=$(grep "work:" "$LOG_FILE" | awk '{print $2}')
            TIME_STR=$(grep "Elapsed (wall clock)" "$LOG_FILE" | awk '{print $8}')

            echo "Done. Load: $LOAD_TP | Work: $WORK_TP ops/s | Time: $TIME_STR"

            # 4. Append to CSV
            echo "${WL_LABEL},${RATIO},${i},${LIMIT_MB},${LOAD_TP},${WORK_TP},${TIME_STR}" >> "$SUMMARY_FILE"

            sleep 2
        done
    done
done

echo ""
echo "=================================================="
echo "Sweep Complete."
echo "Summary saved to: $SUMMARY_FILE"
echo "=================================================="
