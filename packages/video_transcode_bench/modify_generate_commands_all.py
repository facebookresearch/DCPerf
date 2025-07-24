#!/usr/bin/env python3
import argparse
import os
import re
import subprocess
import sys


def parse_arguments():
    """Parse command line arguments."""
    parser = argparse.ArgumentParser(
        description="Modify generate_commands_all.py script."
    )
    parser.add_argument(
        "--sample-rate",
        required=True,
        help="Sample rate for video clip selection (0.0-1.0)",
    )
    parser.add_argument(
        "--sampling-seed", required=True, help="Random seed for reproducible sampling"
    )
    parser.add_argument("--lp-number", required=True, help="Parallelism level setting")
    parser.add_argument("--num-pool", required=True, help="Number of parallel jobs")
    parser.add_argument("--range", required=True, help="Encoder modes range")
    parser.add_argument(
        "--encoder", required=True, help="Encoder type (svt, x264, aom)"
    )
    return parser.parse_args()


def modify_get_clip_list():
    """Modify the get_clip_list function to add caching and sampling."""
    # Read the original file
    with open("./generate_commands_all.py", "r") as f:
        content = f.read()

    # Define the original function pattern with a regex
    pattern = r"def get_clip_list\(clip_dir\):(\n.*?)*?return clip_lists"

    # Define the replacement function with caching and sampling
    replacement = """def get_clip_list(clip_dir):
    # Check if we already have the result in cache
    global clip_lists_cache
    if clip_dir in clip_lists_cache:
        return clip_lists_cache[clip_dir]
    if not os.path.exists(clip_dir):
        raise FileNotFoundError(f"Error: Directory '{clip_dir}' does not exist")
    elif not os.path.isdir(clip_dir):
        raise NotADirectoryError(f"Error: '{clip_dir}' exists but is not a directory")
    elif len(os.listdir(clip_dir)) == 0:
        raise ValueError(f"Error: Directory '{clip_dir}' is empty")


    files = os.listdir(clip_dir)
    files.sort(key=lambda f: get_fps(clip_dir, f), reverse=True)
    files.sort(key=lambda f: f.lower())
    files.sort(key=lambda f: (os.stat(os.path.join(clip_dir, f)).st_size), reverse=True)
    Y4M_HEADER_SIZE = 80

    sample_rate_float = float(SAMPLE_RATE)
    # assign a seed value to the random number generator
    random.seed(SAMPLE_SEED)

    ## sort all files in lists by size
    clip_lists = [[]]
    clip_list = []
    size_0 = os.stat(os.path.join(clip_dir, files[0])).st_size
    for file_ in files:
        if (".yuv" in file_ or ".y4m" in file_):
            # get a random number between 0 and 1
            random_number = random.random()
            if random_number < sample_rate_float:
                if ".yuv" in file_ and os.stat(os.path.join(clip_dir, file_)).st_size == size_0:
                    clip_list.append(file_)
                elif ".y4m" in file_ and size_0 - Y4M_HEADER_SIZE<= os.stat(os.path.join(clip_dir, file_)).st_size<= size_0+Y4M_HEADER_SIZE:
                    clip_list.append(file_)
                else:
                    clip_lists.append(clip_list)
                    clip_list = []
                    size_0 = os.stat(os.path.join(clip_dir, file_)).st_size
                    clip_list.append(file_)

    clip_lists.append(clip_list)

    # Cache the result
    clip_lists_cache[clip_dir] = clip_lists
    return clip_lists"""

    # Replace the function using regex
    modified_content = re.sub(pattern, replacement, content, flags=re.DOTALL)

    # Write the modified content back to the file
    with open("./generate_commands_all.py", "w") as f:
        f.write(modified_content)


def add_sampling_configuration(sample_rate, sample_seed):
    """Add sampling and caching configuration to the script."""
    # Remove any existing SAMPLE_RATE lines
    subprocess.run("sed -i '/^SAMPLE_RATE/d' ./generate_commands_all.py", shell=True)
    subprocess.run("sed -i '/^SAMPLE_SEED/d' ./generate_commands_all.py", shell=True)
    subprocess.run(
        "sed -i '/^clip_lists_cache/d' ./generate_commands_all.py", shell=True
    )

    # Add SAMPLE_RATE after bitstream_folders line
    subprocess.run(
        f"sed -i '/^bitstream\\_folders/a SAMPLE_RATE={sample_rate}' ./generate_commands_all.py",
        shell=True,
    )

    # Add sample_seed after SAMPLE_RATE line
    subprocess.run(
        f"sed -i '/^SAMPLE_RATE/a SAMPLE_SEED={sample_seed}' ./generate_commands_all.py",
        shell=True,
    )

    # Add "clip_lists_cache = {}" after SAMPLE_RATE line
    subprocess.run(
        "sed -i '/^SAMPLE_RATE/a clip_lists_cache = {}' ./generate_commands_all.py",
        shell=True,
    )

    # Add "import random" after import os line
    subprocess.run(
        "sed -i '/^import os/a import random' ./generate_commands_all.py", shell=True
    )


def add_encoder_configuration(encoder):
    """Add encoder-specific configuration to the script and return the run script name."""
    # Remove any existing ENCODER lines
    subprocess.run("sed -i '/^ENCODER/d' ./generate_commands_all.py", shell=True)

    # Set encoder-specific configurations
    if encoder == "svt":
        subprocess.run(
            "sed -i '/^bitstream\\_folders/a ENCODER=\"ffmpeg-svt\"' ./generate_commands_all.py",
            shell=True,
        )
    elif encoder == "x264":
        subprocess.run(
            "sed -i '/^bitstream\\_folders/a ENCODER=\"ffmpeg-x264\"' ./generate_commands_all.py",
            shell=True,
        )

    elif encoder == "aom":
        subprocess.run(
            "sed -i '/^bitstream\\_folders/a ENCODER=\"ffmpeg-libaom\"' ./generate_commands_all.py",
            shell=True,
        )

    else:
        print(f"Error: Unsupported encoder '{encoder}'")
        sys.exit(1)


def main():
    # Parse command line arguments
    args = parse_arguments()

    # Modify the get_clip_list function
    modify_get_clip_list()

    # Add sampling configuration
    add_sampling_configuration(args.sample_rate, args.sampling_seed)
    # running
    subprocess.run("sed -i '/^ENC/d' ./generate_commands_all.py", shell=True)
    subprocess.run("sed -i '/^num_pool/d' ./generate_commands_all.py", shell=True)
    subprocess.run("sed -i '/^lp_number/d' ./generate_commands_all.py", shell=True)
    # Add encoder-specific configuration and get the run script name
    add_encoder_configuration(args.encoder)

    # Add the lp_number, range, and num_pool directly from the command line
    subprocess.run(
        f"sed -i '/^CLIP\\_DIRS/a {args.lp_number}' ./generate_commands_all.py",
        shell=True,
    )
    subprocess.run(
        f"sed -i '/^CLIP\\_DIRS/a {args.range}' ./generate_commands_all.py", shell=True
    )
    subprocess.run(
        f"sed -i '/^CLIP\\_DIRS/a {args.num_pool}' ./generate_commands_all.py",
        shell=True,
    )

    return 0


if __name__ == "__main__":
    sys.exit(main())
