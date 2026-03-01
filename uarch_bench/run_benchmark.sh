#!/bin/bash

# Script to run frontend_study with various parameters and collect performance metrics
# Input: Text file with comma-separated values (one configuration per line)
# Output: CSV format with performance metrics

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <input_file>"
    exit 1
fi

INPUT_FILE="$1"

if [ ! -f "$INPUT_FILE" ]; then
    echo "Error: Input file '$INPUT_FILE' not found"
    exit 1
fi

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXECUTABLE="$SCRIPT_DIR/frontend_study"

if [ ! -x "$EXECUTABLE" ]; then
    echo "Error: Executable '$EXECUTABLE' not found or not executable"
    exit 1
fi

# Print CSV header
echo "buffer_size_mb,num_buffers,function_size_bytes,elapsed_time,iterations_per_sec,cycles,instructions,l1_cache_misses_per_iter,l1_tlb_misses_per_iter,branch_misses_per_iter"

# Process each line in the input file
while IFS= read -r line; do
    # Skip empty lines
    [[ -z "$line" ]] && continue

    # Parse comma-separated values
    IFS=',' read -r divisor iterations buffer_size num_buffers page_size func_sel random_jumps <<< "$line"

    # Trim whitespace
    divisor=$(echo "$divisor" | xargs)
    iterations=$(echo "$iterations" | xargs)
    buffer_size=$(echo "$buffer_size" | xargs)
    num_buffers=$(echo "$num_buffers" | xargs)
    page_size=$(echo "$page_size" | xargs)
    func_sel=$(echo "$func_sel" | xargs)
    random_jumps=$(echo "$random_jumps" | xargs)

    # Run the executable with the parsed parameters
    RUN_OUTPUT=$($EXECUTABLE -d $divisor -i $iterations -b $buffer_size -n $num_buffers -s $page_size -f $func_sel -r $random_jumps 2>&1)

    # Extract performance metrics from the output
    FUNCTION_SIZE=$(echo "$RUN_OUTPUT" | grep -oP 'Estimated function size:\s+\K[0-9]+' || echo "0")
    ELAPSED_TIME=$(echo "$RUN_OUTPUT" | grep -oP 'Elapsed time:\s+\K[0-9.]+' || echo "0")
    ITER_PER_SEC=$(echo "$RUN_OUTPUT" | grep -oP 'Iterations per second:\s+\K[0-9.]+' || echo "0")

    # Get cycles and instructions
    CYCLES=$(echo "$RUN_OUTPUT" | grep -oP 'Cycles:\s+\K[0-9]+' | tr -d ',' | head -n 1 || echo "0")
    INSTRUCTIONS=$(echo "$RUN_OUTPUT" | grep -oP 'Instructions:\s+\K[0-9]+' | tr -d ',' | head -n 1 || echo "0")

    # Get L1 cache misses, L1 TLB misses, and branch misses
    L1_CACHE_MISSES_PER_ITER=$(echo "$RUN_OUTPUT" | grep -oP 'L1I Misses / Iteration:\s+\K[0-9]+(?:\.[0-9]+)?' | tr -d ',' || echo "0")
    L1_TLB_MISSES_PER_ITER=$(echo "$RUN_OUTPUT" | grep -oP 'iTLB Misses / Iteration:\s+\K[0-9]+(?:\.[0-9]+)?' | tr -d ',' || echo "0")
    BRANCH_MISSES_PER_ITER=$(echo "$RUN_OUTPUT" | grep -oP 'Branch Misses / Iteration:\s+\K[0-9]+(?:\.[0-9]+)?' | tr -d ',' || echo "0")

    # Output CSV row
    echo "$buffer_size,$num_buffers,$FUNCTION_SIZE,$ELAPSED_TIME,$ITER_PER_SEC,$CYCLES,$INSTRUCTIONS,$L1_CACHE_MISSES_PER_ITER,$L1_TLB_MISSES_PER_ITER,$BRANCH_MISSES_PER_ITER"

done < "$INPUT_FILE"
