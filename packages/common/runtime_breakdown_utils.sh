#!/bin/bash

# Runtime breakdown utilities for benchmark tracking

# Function to create a CSV file for runtime breakdown tracking
# Usage: create_breakdown_csv <folder_path>
breakdown_file_name="breakdown.csv"
# By using a variable here and putting the name (string) in it,
# we avoids repeating the string in
# all other benchmarks and avoid mistakes like typos in string.
main_operation_name="main_benchmark"
create_breakdown_csv() {
    local folder_path="$1"

    if [ -z "$folder_path" ]; then
        echo "Error: Folder path is required" >&2
        return 1
    fi

    # Create the folder if it doesn't exist
    mkdir -p "$folder_path"

    local csv_file="${folder_path}/${breakdown_file_name}"

    # Create CSV file with headers
    if echo "operation_name,PID,timestamp_type,timestamp" > "$csv_file"; then
        echo "Created breakdown CSV file: $csv_file"
        return 0
    else
        echo "Error: Failed to create CSV file: $csv_file" >&2
        return 1
    fi
}

# Function to log an entry to the breakdown CSV file
# Usage: log_breakdown_entry <folder_path> <operation_name> <pid> <timestamp_type> [timestamp]
log_breakdown_entry() {
    local folder_path="$1"
    local operation_name="$2"
    local pid="$3"
    local timestamp_type="$4"  # e.g., "start" or "end"
    local timestamp="$5"

    if [ -z "$folder_path" ] || [ -z "$operation_name" ] || [ -z "$pid" ] || [ -z "$timestamp_type" ]; then
        echo "Error: folder_path, operation_name, pid, and timestamp_type are required" >&2
        return 1
    fi

    local csv_file="${folder_path}/${breakdown_file_name}"

    # Create CSV if it doesn't exist
    if [ ! -f "$csv_file" ]; then
        create_breakdown_csv "$folder_path"
    fi

    # Use current timestamp if not provided
    if [ -z "$timestamp" ]; then
        timestamp=$(date '+%Y-%m-%d %H:%M:%S.%3N')
    fi

    # Append entry to CSV file
    if echo "$operation_name,$pid,$timestamp_type,$timestamp" >> "$csv_file"; then
        echo "Logged entry: $operation_name ($timestamp_type) at $timestamp"
        return 0
    else
        echo "Error: Failed to log entry to CSV file: $csv_file" >&2
        return 1
    fi
}

# Helper function to log start of an operation
# Usage: log_start <folder_path> <operation_name> <pid>
log_start() {
    log_breakdown_entry "$1" "$2" "$3" "start"
}

# Helper function to log end of an operation
# Usage: log_end <folder_path> <operation_name> <pid>
log_end() {
    log_breakdown_entry "$1" "$2" "$3" "end"
}

# Helper function to log start of main_benchmark operation
# Usage: log_main_benchmark_start <folder_path> <pid>
log_main_benchmark_start() {
    log_breakdown_entry "$1" "$main_operation_name" "$2" "start"
}

# Helper function to log end of main_benchmark operation
# Usage: log_main_benchmark_end <folder_path> <pid>
log_main_benchmark_end() {
    log_breakdown_entry "$1" "$main_operation_name" "$2" "end"
}
