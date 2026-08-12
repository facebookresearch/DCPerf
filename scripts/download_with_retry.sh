#!/bin/bash

# Function to download a file with retry logic
# Usage: download_with_retry <url> [output_filename] [max_retries]
download_with_retry() {
    local url="$1"
    local output="${2:-}"
    local max_retries="${3:-5}"
    local retry_count=0
    local success=false

    echo "Downloading from: $url"

    while [ $retry_count -lt "$max_retries" ] && [ "$success" = false ]; do
        retry_count=$((retry_count + 1))
        echo "Attempt $retry_count of $max_retries"

        if [ -n "$output" ]; then
            if wget -O "$output" "$url"; then
                success=true
                echo "Download successful on attempt $retry_count"
            else
                echo "Download failed on attempt $retry_count"
                if [ $retry_count -lt "$max_retries" ]; then
                    echo "Retrying in 5 seconds..."
                    sleep 5
                fi
            fi
        else
            if wget "$url"; then
                success=true
                echo "Download successful on attempt $retry_count"
            else
                echo "Download failed on attempt $retry_count"
                if [ $retry_count -lt "$max_retries" ]; then
                    echo "Retrying in 5 seconds..."
                    sleep 5
                fi
            fi
        fi
    done

    if [ "$success" = false ]; then
        echo "ERROR: Failed to download $url after $max_retries attempts"
        return 1
    fi

    return 0
}

# Function to download with curl and retry logic
# Usage: curl_with_retry <url> [output_filename] [max_retries]
curl_with_retry() {
    local url="$1"
    local output="${2:-}"
    local max_retries="${3:-5}"
    local retry_count=0
    local success=false

    echo "Downloading from: $url"

    while [ $retry_count -lt "$max_retries" ] && [ "$success" = false ]; do
        retry_count=$((retry_count + 1))
        echo "Attempt $retry_count of $max_retries"

        if [ -n "$output" ]; then
            if curl "$(fwdproxy-config curl)" -O "$output" "$url"; then
                success=true
                echo "Download successful on attempt $retry_count"
            else
                echo "Download failed on attempt $retry_count"
                if [ $retry_count -lt "$max_retries" ]; then
                    echo "Retrying in 5 seconds..."
                    sleep 5
                fi
            fi
        else
            if curl "$(fwdproxy-config curl)" -O "$url"; then
                success=true
                echo "Download successful on attempt $retry_count"
            else
                echo "Download failed on attempt $retry_count"
                if [ $retry_count -lt "$max_retries" ]; then
                    echo "Retrying in 5 seconds..."
                    sleep 5
                fi
            fi
        fi
    done

    if [ "$success" = false ]; then
        echo "ERROR: Failed to download $url after $max_retries attempts"
        return 1
    fi

    return 0
}
