#!/usr/bin/env python3
"""
Script to monitor vm-tcspace and vm-dump-tc.
Usage: python dump_jit_size.py [options]

Options:
  --output-dir DIR          Directory to save all output files (default: /tmp/jit_study_output)
  --dump-jit-interval SECONDS How often to update the CSV file (default: 1)
  --ip-and-port PORT        IP and port to use for the request (default: localhost:9092)
  --dump-tc                 Enable dumping of TC (translation cache) using vm-dump-tc
  --dump-tc-interval SECONDS How often to dump TC (default: 600)
"""

import argparse
import csv
import datetime
import os
import re
import shutil
import signal
import subprocess
import sys
import time


def get_timestamp():
    """Return current timestamp in readable format."""
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def get_timestamp_filename():
    """Return timestamp suitable for filenames."""
    return datetime.datetime.now().strftime("%Y%m%d_%H%M%S")


def check_curl_installed():
    """Check if curl is installed."""
    if shutil.which("curl") is None:
        print("Error: curl is not installed. Please install curl.")
        sys.exit(1)


def run_curl(ip_and_port, endpoint="/vm-tcspace"):
    """Run curl command and return output and status."""
    try:
        # Run curl with a timeout of 5 seconds
        result = subprocess.run(
            [
                "curl",
                "-s",
                "--max-time",
                "5",
                f"http://{ip_and_port}{endpoint}",
            ],
            capture_output=True,
            text=True,
            timeout=6,  # Slightly longer than curl's timeout
        )
        return result.stdout.strip(), result.returncode
    except subprocess.TimeoutExpired:
        return "", 28  # 28 is curl's timeout error code


def dump_tc(ip_and_port, output_dir, timestamp):
    """Run vm-dump-tc command and copy dumped files to the output directory."""
    print(f"[{timestamp}] Dumping TC...")
    output, status = run_curl(ip_and_port, "/vm-dump-tc")

    if status == 0:
        # Create TC dumps directory if needed
        timestamp = get_timestamp_filename()
        tc_dumps_dir = os.path.join(output_dir, f"tc_dumps_{timestamp}")
        os.makedirs(tc_dumps_dir, exist_ok=True)
        print(f"TC dumps is saved to: {tc_dumps_dir}")

        # Copy anything in the /tmp/ folder with the tc_dump_* and the tc_data.txt.gz into the tc_dumps_dir
        for file in os.listdir("/tmp/"):
            if file.startswith("tc_dump_") or file == "tc_data.txt.gz":
                shutil.copyfile(f"/tmp/{file}", os.path.join(tc_dumps_dir, file))
        print(f"[{timestamp}] TC dumped successfully.")
        return True
    else:
        # Different error messages based on status code
        if status == 6:
            error_msg = "Could not resolve host"
        elif status == 7:
            error_msg = "Failed to connect"
        elif status == 28:
            error_msg = "Operation timed out"
        else:
            error_msg = f"Error (code: {status})"

        error_output = f"[{timestamp}] ERROR dumping TC: {error_msg} - http://{ip_and_port}/vm-dump-tc"

        print(error_output)
        return False


def extract_jit_size(output):
    """Extract the JIT size from the output."""
    # Look for the line containing "in total"
    for line in output.split("\n"):
        if "in total" in line:
            # Extract the number before "bytes"
            match = re.search(r"mcg:\s+(\d+)\s+bytes", line)
            if match:
                return int(match.group(1))
    return None


def append_to_csv(csv_file, jit_data):
    """Append JIT size data to CSV file."""
    with open(csv_file, "a", newline="") as f:
        writer = csv.writer(f)
        for timestamp, elapsed_secs, jit_size in jit_data:
            writer.writerow([timestamp, elapsed_secs, jit_size])


def handle_exit(_signum, _frame):
    """Handle exit signals gracefully."""

    global running
    # Set running flag to False to stop the main loop
    running = False

    # Don't call sys.exit() here as it prevents the main loop from cleaning up
    # The main loop will exit naturally when running becomes False


def ensure_directory_exists(file_path):
    """Ensure the directory for the given file path exists."""
    directory = os.path.dirname(file_path)
    if directory and not os.path.exists(directory):
        try:
            os.makedirs(directory, exist_ok=True)
            print(f"Created directory: {directory}")
        except Exception as e:
            print(f"Error creating directory {directory}: {e}")
            sys.exit(1)


def parse_args():
    """Parse command line arguments."""
    # Default directory for output files
    default_dir = "/tmp/jit_study_output"

    parser = argparse.ArgumentParser(description="Monitor vm-tcspace and vm-dump-tc.")
    parser.add_argument(
        "--output-dir",
        default=default_dir,
        help=f"Directory to save all output files (default: {default_dir})",
    )
    parser.add_argument(
        "--dump-jit-interval",
        type=int,
        default=1,
        help="How often to update the CSV file (in seconds, default: 1)",
    )
    parser.add_argument(
        "--ip-and-port",
        type=str,
        default="localhost:9092",
        help="IP and port to use for the request (default: localhost:9092)",
    )
    parser.add_argument(
        "--dump-tc",
        action="store_true",
        help="Enable dumping of TC (translation cache) using vm-dump-tc",
    )
    parser.add_argument(
        "--dump-tc-interval",
        type=int,
        default=600,
        help="How often to dump TC (in seconds, default: 600)",
    )

    return parser.parse_args()


# Global variables for signal handler
running = True


def format_and_dump_jit_output(
    request_output,
    request_status,
    timestamp,
    elapsed_secs,
    txt_file,
    csv_file,
    ip_and_port,
):
    # Format the output with timestamp
    if request_status == 0:
        # Extract JIT size if request was successful
        jit_size = extract_jit_size(request_output)

        if jit_size is not None:
            # Add to JIT data and write to CSV
            append_to_csv(csv_file, [(timestamp, elapsed_secs, jit_size)])
            output = f"[{timestamp}] JIT Size: {jit_size} bytes\n{request_output}"
        else:
            output = f"[{timestamp}] Could not extract JIT size\n{request_output}"
    else:
        # Different error messages based on status code
        if request_status == 6:
            error_msg = "Could not resolve host"
        elif request_status == 7:
            error_msg = "Failed to connect"
        elif request_status == 28:
            error_msg = "Operation timed out"
        else:
            error_msg = f"Error (code: {request_status})"

        output = f"[{timestamp}] ERROR: {error_msg} - http://{ip_and_port}/vm-tcspace"

    # Output to file or terminal
    if txt_file:
        with open(txt_file, "a") as f:
            f.write(output + "\n")
    else:
        print(output)


def run_monitoring_loop(
    output_dir,
    dump_jit_interval,
    ip_and_port,
    dump_tc_enabled,
    dump_tc_interval,
):
    """Run the monitoring loop."""
    global running

    ensure_directory_exists(output_dir)

    # Generate filenames with timestamp
    timestamp = get_timestamp_filename()
    txt_file = os.path.join(output_dir, f"jit_monitor_{timestamp}.txt")
    csv_file = os.path.join(output_dir, f"jit_monitor_{timestamp}.csv")

    # Clear the output file if it exists, because from now on we will be appending to it
    open(txt_file, "w").close()

    # Create CSV with header
    with open(csv_file, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["Timestamp", "Elapsed Time (s)", "JIT Size (bytes)"])

    try:
        print("Starting monitoring.")
        print("Press Ctrl+C to stop monitoring.")
        start_time = time.time()
        last_tc_dump_time = start_time
        last_jit_query_time = start_time

        while running:
            current_time = time.time()
            elapsed_secs = int(current_time - start_time)
            timestamp = get_timestamp()

            # Run vm-tcspace to get JIT size
            last_jit_query_time = current_time
            request_output, request_status = run_curl(ip_and_port)
            format_and_dump_jit_output(
                request_output,
                request_status,
                timestamp,
                elapsed_secs,
                txt_file,
                csv_file,
                ip_and_port,
            )

            # Check if it's time to dump TC
            if (
                dump_tc_enabled
                and (current_time - last_tc_dump_time) >= dump_tc_interval
            ):
                dump_tc(ip_and_port, output_dir, timestamp)
                last_tc_dump_time = current_time

            # sometimes the request waits for a long time before returning
            # So, we need to calculate the sleep time based on when the next query should happen
            sleep_time = (last_jit_query_time + dump_jit_interval) - time.time()

            # Sleep for the remaining time
            if sleep_time > 0:
                time.sleep(sleep_time)

    except KeyboardInterrupt:
        # Handle Ctrl+C gracefully
        print("\nMonitoring jit size stopped by user (Ctrl+C).")
    finally:
        print("Monitoring stopped. Data saved to:")
        print(f"  - Text file: {txt_file}")
        print(f"  - CSV file: {csv_file}")


def main():
    """Main function to run the monitoring loop."""
    global running
    # Register signal handlers for graceful termination
    signal.signal(signal.SIGINT, handle_exit)  # Ctrl+C
    signal.signal(signal.SIGTERM, handle_exit)  # Termination signal

    args = parse_args()
    output_dir = args.output_dir
    dump_jit_interval = args.dump_jit_interval
    ip_and_port = args.ip_and_port
    dump_tc_enabled = args.dump_tc
    dump_tc_interval = args.dump_tc_interval

    # Check if curl is installed
    check_curl_installed()

    # Make sure output directory exists
    os.makedirs(output_dir, exist_ok=True)

    # Print configuration
    print("JIT monitoring configuration:")
    print(f"  - Output directory: {output_dir}")
    print(f"  - JIT monitoring interval: {dump_jit_interval} seconds")
    print(f"  - Server: {ip_and_port}")
    print(f"  - Dump TC: {dump_tc_enabled}")
    print(f"  - TC dump interval: {dump_tc_interval} seconds")
    run_monitoring_loop(
        output_dir,
        dump_jit_interval,
        ip_and_port,
        dump_tc_enabled,
        dump_tc_interval,
    )
    print(f"Jit monitoring data is stored in {output_dir}")
    return 0


if __name__ == "__main__":
    main()
