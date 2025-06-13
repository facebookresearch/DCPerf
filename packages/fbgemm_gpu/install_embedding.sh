#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


################################################################################
# Global Configuration Variables
################################################################################

# Directory where benchmark executables will be stored
BENCHMARKS_DIR="$(pwd)/benchmarks/fbgemm_embedding"

# Path to Miniconda installation
MINICONDA_PREFIX=$(pwd)/build/miniconda

# Version of FBGEMM to install
FBGEMM_VERSION=v1.2.0

# Version of PyTorch to install
PYTORCH_VERSION=2.7.0

# Name of the conda environment to create
BUILD_ENV=fbgemm_build_oss_env

# Python version to use
PYTHON_VERSION=3.13


################################################################################
# Platform Specific Variables
################################################################################
# Get kernel name (e.g., Linux, Darwin)
# shellcheck disable=SC2155
export KERN_NAME="$(uname -s)"

# Get machine hardware name (e.g., x86_64, aarch64)
# shellcheck disable=SC2155
export MACHINE_NAME="$(uname -m)"

# Combine kernel and machine name (e.g., Linux-x86_64)
# shellcheck disable=SC2155
export PLATFORM_NAME="$KERN_NAME-$MACHINE_NAME"

# Convert kernel name to lowercase for consistency
# shellcheck disable=SC2155
export KERN_NAME_LC="$(echo "$KERN_NAME" | awk '{print tolower($0)}')"

# Convert machine name to lowercase for consistency
# shellcheck disable=SC2155
export MACHINE_NAME_LC="$(echo "$MACHINE_NAME" | awk '{print tolower($0)}')"

# Combine lowercase kernel and machine name (e.g., linux-x86_64)
# shellcheck disable=SC2155
export PLATFORM_NAME_LC="$KERN_NAME_LC-$MACHINE_NAME_LC"


################################################################################
# Miniconda Setup Functions
################################################################################
# These functions handle the installation and configuration of Miniconda
# and resolve common issues with Python packages and dependencies

# Function to fix pyOpenSSL version compatibility issues
__handle_pyopenssl_version_issue () {
  # Parameter: Name of the conda environment
  local env_name="$1"

  # Get the environment prefix format (-n or -p depending on if env_name is a path)
  # shellcheck disable=SC2155
  local env_prefix=$(env_name_or_prefix "${env_name}")

  # The pyOpenSSL and cryptography packages versions need to line up for PyPI publishing to work
  # Reference: https://stackoverflow.com/questions/74981558/error-updating-python3-pip-attributeerror-module-lib-has-no-attribute-openss
  echo "[SETUP] Upgrading pyOpenSSL to version greater than 22.1.0 ..."
  # shellcheck disable=SC2086
  (exec_with_retries 3 conda install ${env_prefix} -c conda-forge --override-channels -y \
    "pyOpenSSL>22.1.0") || return 1

  # Verify the installation by testing the import
  # This test fails with load errors if the pyOpenSSL and cryptography package versions don't align
  echo "[SETUP] Testing pyOpenSSL import to verify installation ..."
  (test_python_import_package "${env_name}" OpenSSL) || return 1
}

# Function to fix missing crypt.h header issue
# This is necessary to prevent runtime errors when using torch.compile()
__handle_libcrypt_header_issue () {
  # Parameter: Name of the conda environment
  local env_name="$1"

  # Get the environment prefix format
  # shellcheck disable=SC2155
  local env_prefix=$(env_name_or_prefix "${env_name}")

  # Install libxcrypt package which provides the necessary cryptography libraries
  # Reference: https://git.sr.ht/~andir/nixpkgs/commit/4ace88d63b14ef62f24d26c984775edc2ab1737c
  echo "[SETUP] Installing libxcrypt package for cryptography support ..."
  # shellcheck disable=SC2086
  (exec_with_retries 3 conda install ${env_prefix} -c conda-forge --override-channels -y \
    libxcrypt) || return 1

  # Get the conda environment prefix path
  # shellcheck disable=SC2155,SC2086
  local conda_prefix=$(conda run ${env_prefix} printenv CONDA_PREFIX)

  # Get the Python version installed in the environment
  # shellcheck disable=SC2207,SC2086
  local python_version=($(conda run --no-capture-output ${env_prefix} python --version))

  # Split the Python version string into an array (e.g., "Python 3.13.0" -> [3, 13, 0])
  # shellcheck disable=SC2206
  local python_version_arr=(${python_version[1]//./ })

  # Copy the crypt.h header file to the Python-specific include directory
  # This is needed because some Python modules look for it in the Python-specific include path
  # Reference: https://github.com/stanford-futuredata/ColBERT/issues/309
  echo "[SETUP] Copying crypt.h header to Python-specific include directory ..."
  # shellcheck disable=SC2206
  local dst_file="${conda_prefix}/include/python${python_version_arr[0]}.${python_version_arr[1]}/crypt.h"
  print_exec cp "${conda_prefix}/include/crypt.h" "${dst_file}"
}

# Function to set up Miniconda in the specified directory
# This handles downloading, installing, and initializing Miniconda
setup_miniconda() {
  # Parameter: Directory where Miniconda should be installed
  local miniconda_prefix="$1"

  echo "Setting up Miniconda at ${miniconda_prefix}..."

  # Remove existing installation if it exists to ensure a clean setup
  if [ -f "${miniconda_prefix}/bin/conda" ]; then
    echo "Removing existing Miniconda installation to ensure clean setup..."
    rm -rf "${miniconda_prefix}"
  fi

  # Create the directory for Miniconda installation
  mkdir -p "$miniconda_prefix"

  # Download the appropriate Miniconda installer for the current platform
  echo "Downloading Miniconda installer for ${PLATFORM_NAME}..."
  wget -q "https://repo.anaconda.com/miniconda/Miniconda3-latest-${PLATFORM_NAME}.sh" -O miniconda.sh

  # Install Miniconda in batch mode (-b) to the specified prefix (-p) and update (-u) any existing installation
  echo "Installing Miniconda in batch mode..."
  bash miniconda.sh -b -p "$miniconda_prefix" -u
  rm -f miniconda.sh

  # Initialize conda in the current shell to make conda commands available
  echo "Initializing conda in the current shell for immediate use..."
  eval "$("${miniconda_prefix}/bin/conda" shell.bash hook)"

  # Update PATH and set CONDA environment variable for future reference
  export PATH="${miniconda_prefix}/bin:${PATH}"
  export CONDA="${miniconda_prefix}"

  # Update conda to the latest version from conda-forge
  echo "Updating conda to the latest version..."
  conda update -n base -c conda-forge -y conda

  echo "Miniconda setup complete and ready for use!"
}


# Function to create a new conda environment with the specified Python version
# This handles environment creation, package installation, and common issue fixes
create_conda_environment () {
  # Parameters:
  # $1: Name of the conda environment to create
  # $2: Python version to install (e.g., "3.13")
  local env_name="$1"
  # shellcheck disable=SC2178
  local python_version="$2"

  # shellcheck disable=SC2128
  if [ "$python_version" == "" ]; then
    echo "Usage: ${FUNCNAME[0]} ENV_NAME PYTHON_VERSION"
    echo "Example:"
    echo "    ${FUNCNAME[0]} build_env 3.10"
    return 1
  else
    # Print header with timestamp for logging purposes
    echo "################################################################################"
    echo "# Create Conda Environment"
    echo "#"
    echo "# [$(date --utc +%FT%T.%3NZ)] + ${FUNCNAME[0]} ${*}"
    echo "################################################################################"
    echo ""
  fi

  # Verify network connectivity before proceeding
  test_network_connection || return 1

  # Display existing conda environments for reference
  echo "[SETUP] Listing existing Conda environments for reference..."
  print_exec conda info --envs

  # Handle potential conda error: "CondaValueError: Value error: prefix already exists"
  # We resolve this by pre-emptively deleting the environment directory if it exists
  # Reference: https://stackoverflow.com/questions/40180652/condavalueerror-value-error-prefix-already-exists
  echo "[SETUP] Deleting the prefix directory if it exists to prevent conda errors..."
  # shellcheck disable=SC2155
  local conda_prefix=$(conda run -n base printenv CONDA_PREFIX)
  print_exec rm -rf "${conda_prefix}/envs/${env_name}"

  # Get the environment prefix format (-n or -p)
  # shellcheck disable=SC2155
  local env_prefix=$(env_name_or_prefix "${env_name}")

  # Create the new conda environment with the specified Python version
  # shellcheck disable=SC2128
  echo "[SETUP] Creating new Conda environment (Python ${python_version}) from conda-forge..."
  # shellcheck disable=SC2086,SC2128
  (exec_with_retries 3 conda create -y ${env_prefix} -c conda-forge python="${python_version}") || return 1

  # Update pip to the latest version to ensure compatibility with newer packages
  echo "[SETUP] Upgrading PIP to latest version for better package compatibility..."
  # shellcheck disable=SC2086
  (exec_with_retries 3 conda run ${env_prefix} pip install --upgrade pip) || return 1

  # Handle pyOpenSSL version compatibility issues
  __handle_pyopenssl_version_issue "${env_name}"

  # Handle missing crypt.h header issue
  __handle_libcrypt_header_issue "${env_name}"

  # Verify the Python version that was installed
  # shellcheck disable=SC2086,SC2128
  echo "[SETUP] Installed Python version: $(conda run ${env_prefix} python --version)"
  echo "[SETUP] Successfully created Conda environment: ${env_name}"
}

################################################################################
# Build Tools Setup Functions
################################################################################
# These functions handle the installation and configuration of compilers and
# build tools necessary for building FBGEMM and other C++ libraries

# Function to determine the appropriate compiler architecture name based on the machine architecture
# This is used to select the correct compiler packages from conda-forge
__extract_compiler_archname () {
  # For x86_64 architecture, use "64" as the compiler architecture name
  # This is the naming convention used by conda-forge packages
  if [ "$MACHINE_NAME_LC" = "x86_64" ]; then
    export COMPILER_ARCHNAME="64"
  # For ARM architectures (aarch64 or arm64), use "aarch64" as the compiler architecture name
  elif [ "$MACHINE_NAME_LC" = "aarch64" ] || [ "$MACHINE_NAME_LC" = "arm64" ]; then
    export COMPILER_ARCHNAME="aarch64"
  # For any other architecture, use the machine name directly
  else
    export COMPILER_ARCHNAME="$MACHINE_NAME_LC"
  fi
}


# Function to install the appropriate GLIBC version for the compiler
# This is necessary to ensure compatibility between the compiler and the system libraries
__conda_install_glibc () {
  # sysroot_linux-<arch> needs to be installed alongside the C/C++ compiler for GLIBC

  # Get the environment prefix format (-n or -p)
  # shellcheck disable=SC2155
  local env_prefix=$(env_name_or_prefix "${env_name}")

  # Get the GCC version from environment variable or use default 10.4.0
  # shellcheck disable=SC2155
  local gcc_version="${GCC_VERSION:-10.4.0}"

  echo "[INSTALL] Installing GLIBC (architecture = ${COMPILER_ARCHNAME}) ..."

  # Split the GCC version string into an array (e.g., "10.4.0" -> [10, 4, 0])
  # shellcheck disable=SC2206
  # local gcc_version_arr=(${gcc_version//./ })
  local glibc_version="${GLIBC_VERSION:-2.17}"

  # Install sysroot_linux with the specified GLIBC version
  # shellcheck disable=SC2086
  (exec_with_retries 3 conda install ${env_prefix} -c conda-forge --override-channels -y \
    "sysroot_linux-${COMPILER_ARCHNAME}=${glibc_version}") || return 1

  # Display the LD_LIBRARY_PATH to verify library paths are set correctly
  echo "[CHECK] LD_LIBRARY_PATH = ${LD_LIBRARY_PATH}"
}

# Function to install GCC compiler from conda-forge
# This installs the C/C++ compiler and sets up necessary symlinks
__conda_install_gcc () {
  # Install gxx_linux-<arch> from conda-forge instead of from anaconda channel
  # conda-forge provides more up-to-date and better maintained compiler packages

  # Get the environment prefix format (-n or -p)
  # shellcheck disable=SC2155
  local env_prefix=$(env_name_or_prefix "${env_name}")

  # Get the GCC version from environment variable or use default 10.4.0
  # shellcheck disable=SC2155
  local gcc_version="${GCC_VERSION:-10.4.0}"

  # Install GCC with the specified version for the current architecture
  echo "[INSTALL] Installing GCC (${gcc_version}, ${COMPILER_ARCHNAME}) through Conda ..."
  # shellcheck disable=SC2086
  (exec_with_retries 3 conda install ${env_prefix} -c conda-forge --override-channels -y \
    "gxx_linux-${COMPILER_ARCHNAME}"=${gcc_version}) || return 1

  # We need to create symlinks to standard names like cc, gcc, c++, g++
  echo "[INSTALL] Setting the C/C++ compiler symlinks for easier access ..."

  # Get the paths to the C and C++ compilers from the environment
  # shellcheck disable=SC2155,SC2086
  local cc_path=$(conda run ${env_prefix} printenv CC)
  # shellcheck disable=SC2155,SC2086
  local cxx_path=$(conda run ${env_prefix} printenv CXX)

  # Create symlinks to standard compiler names, overriding existing symlinks if needed
  print_exec ln -sf "${cc_path}" "$(dirname "$cc_path")/cc"
  print_exec ln -sf "${cc_path}" "$(dirname "$cc_path")/gcc"
  print_exec ln -sf "${cxx_path}" "$(dirname "$cxx_path")/c++"
  print_exec ln -sf "${cxx_path}" "$(dirname "$cxx_path")/g++"

  # If the SET_GLIBCXX_PRELOAD environment variable is set to 1,
  # set up libstdc++ preload options for compatibility
  if [ "$SET_GLIBCXX_PRELOAD" == "1" ]; then
    # Set libstdc++ preload options
    __set_glibcxx_preload
  fi
}

# Function to install Clang compiler from conda-forge
# This installs the Clang/LLVM toolchain and sets up necessary symlinks and environment variables
__conda_install_clang () {
  # Get the environment prefix format (-n or -p)
  # shellcheck disable=SC2155
  local env_prefix=$(env_name_or_prefix "${env_name}")

  # Get the LLVM version from environment variable or use default 16.0.6
  # shellcheck disable=SC2155
  local llvm_version="${LLVM_VERSION:-16.0.6}"

  # Install Clang and related LLVM libraries
  echo "[INSTALL] Installing Clang (${llvm_version}, ${COMPILER_ARCHNAME}) and relevant libraries through Conda ..."

  # NOTE: libcxx from conda-forge is outdated for linux-aarch64, so we cannot
  # explicitly specify the version number for that package
  # shellcheck disable=SC2086
  (exec_with_retries 3 conda install ${env_prefix} -c conda-forge --override-channels -y \
    clangxx=${llvm_version} \
    libcxx \
    llvm-openmp=${llvm_version} \
    compiler-rt=${llvm_version}) || return 1

  # Create symlinks for standard compiler names (cc, c++, etc.)
  echo "[INSTALL] Setting the C/C++ compiler symlinks for Clang ..."
  set_clang_symlinks "${env_name}"

  # Remove the Conda activation scripts for GCC to prevent conflicts with Clang
  __remove_gcc_activation_scripts

  # Set environment variables to use Clang as the default compiler
  # shellcheck disable=SC2086
  print_exec conda env config vars set ${env_prefix} CC="${cc_path}"
  # shellcheck disable=SC2086
  print_exec conda env config vars set ${env_prefix} CXX="${cxx_path}"

  # Verify that the compiler environment variables are set correctly
  # shellcheck disable=SC2086
  print_exec conda run ${env_prefix} printenv CC
  # shellcheck disable=SC2086
  print_exec conda run ${env_prefix} printenv CXX

  # Add the Conda environment's lib directory to the library path
  # This ensures that libraries installed in the Conda environment are found
  # shellcheck disable=SC2155,SC2086
  local conda_prefix=$(conda run ${env_prefix} printenv CONDA_PREFIX)
  append_to_library_path "${env_name}" "${conda_prefix}/lib"
}

# Function to perform post-installation checks for the compiler
# This verifies that the compiler is properly installed and configured
__compiler_post_install_checks () {
  # Get the environment prefix format (-n or -p)
  # shellcheck disable=SC2155
  local env_prefix=$(env_name_or_prefix "${env_name}")

  # Verify that all standard compiler commands are available in the PATH
  echo "[CHECK] Verifying that C/C++ compilers are accessible via standard names..."
  (test_binpath "${env_name}" cc) || return 1
  (test_binpath "${env_name}" gcc) || return 1
  (test_binpath "${env_name}" c++) || return 1
  (test_binpath "${env_name}" g++) || return 1

  # Dump all preprocessor defines from the C compiler
  # This helps verify compiler configuration and available features
  # Reference: https://stackoverflow.com/questions/2224334/gcc-dump-preprocessor-defines
  echo "[INFO] Printing out all preprocessor defines in the C compiler for verification..."
  # shellcheck disable=SC2086
  print_exec conda run ${env_prefix} cc -dM -E -

  # Dump all preprocessor defines from the C++ compiler
  echo "[INFO] Printing out all preprocessor defines in the C++ compiler for verification..."
  # shellcheck disable=SC2086
  print_exec conda run ${env_prefix} c++ -dM -E -x c++ -

  # Print the C++ compiler version for verification
  echo "[INFO] Verifying C++ compiler version..."
  # shellcheck disable=SC2086
  print_exec conda run ${env_prefix} c++ --version

  # Verify that the libstdc++.so.6 library is available
  echo "[CHECK] Verifying libstdc++.so.6 is available..."
  # shellcheck disable=SC2153
  if [ "${CONDA_PREFIX}" == '' ]; then
    echo "[CHECK] CONDA_PREFIX is not set, checking in environment..."
    (test_filepath "${env_name}" 'libstdc++.so.6') || return 1
  else
    echo "[CHECK] Checking for libstdc++.so.6 in CONDA_PREFIX..."
    (test_filepath "${CONDA_PREFIX}" 'libstdc++.so.6') || return 1
  fi

  # Print the default C standard version used by the compiler
  # Reference: https://stackoverflow.com/questions/4991707/how-to-find-my-current-compilers-standard-like-if-it-is-c90-etc
  echo "[INFO] Printing the default version of the C standard used by the compiler..."
  print_exec "conda run ${env_prefix} cc -dM -E - < /dev/null | grep __STDC_VERSION__"

  # Print the default C++ standard version used by the compiler
  # Reference: https://stackoverflow.com/questions/2324658/how-to-determine-the-version-of-the-c-standard-used-by-the-compiler
  echo "[INFO] Printing the default version of the C++ standard used by the compiler..."
  print_exec "conda run ${env_prefix} c++ -dM -E -x c++ - < /dev/null | grep __cplusplus"
}


# Main function to install C/C++ compiler in the specified conda environment
# This function orchestrates the installation of either GCC or Clang compiler
install_cxx_compiler () {
  # Parameters:
  # $1: Name of the conda environment
  # $2: Compiler type to install ("gcc" or "clang")
  env_name="$1"
  local compiler="$2"

  # Validate required parameters
  if [ "$env_name" == "" ]; then
    echo "Usage: ${FUNCNAME[0]} ENV_NAME [COMPILER_TYPE]"
    echo "Example(s):"
    echo "    ${FUNCNAME[0]} build_env clang  # Install C/C++ compilers (clang)"
    echo "    ${FUNCNAME[0]} build_env gcc    # Install C/C++ compilers (gcc)"
    return 1
  else
    # Print header with timestamp for logging purposes
    echo "################################################################################"
    echo "# Install C/C++ Compilers"
    echo "#"
    echo "# [$(date --utc +%FT%T.%3NZ)] + ${FUNCNAME[0]} ${*}"
    echo "################################################################################"
    echo ""
  fi

  # Verify network connectivity before proceeding
  test_network_connection || return 1

  # Determine the appropriate compiler architecture name for the current machine
  __extract_compiler_archname

  # Install GLIBC libraries required by the compiler
  __conda_install_glibc

  # Install GCC and libstdc++
  # NOTE: We unconditionally install libstdc++ here because CUDA only supports
  # libstdc++, even if host compiler is set to Clang:
  #   https://docs.nvidia.com/cuda/cuda-installation-guide-linux/index.html#host-compiler-support-policy
  #   https://forums.developer.nvidia.com/t/cuda-issues-with-clang-compiler/177589/8
  __conda_install_gcc

  # If Clang was specified as the compiler, install it
  if [ "$compiler" == "clang" ]; then
    # Existing symlinks to cc / c++ / gcc / g++ will be overridden with Clang versions
    __conda_install_clang
  fi

  # Perform post-installation checks to verify compiler setup
  __compiler_post_install_checks
  echo "[INSTALL] Successfully installed C/C++ compilers"
}


install_build_tools () {
  local env_name="$1"
  if [ "$env_name" == "" ]; then
    echo "Usage: ${FUNCNAME[0]} ENV_NAME"
    echo "Example(s):"
    echo "    ${FUNCNAME[0]} build_env"
    return 1
  else
    echo "################################################################################"
    echo "# Install Build Tools"
    echo "#"
    echo "# [$(date --utc +%FT%T.%3NZ)] + ${FUNCNAME[0]} ${*}"
    echo "################################################################################"
    echo ""
  fi

  test_network_connection || return 1

  # shellcheck disable=SC2155
  local env_prefix=$(env_name_or_prefix "${env_name}")

  echo "[INSTALL] Installing build tools ..."
  # NOTES:
  #
  # - Only the openblas package will install <cblas.h> directly into
  #   $CONDA_PREFIX/include directory, which is required for FBGEMM tests
  #
  # - ncurses is needed to silence libtinfo6.so errors for ROCm+Clang builds
  # - rhash is needed bc newer versions of GXX package don't come packaged with this library anymore
  #
  # shellcheck disable=SC2086
  (exec_with_retries 3 conda install ${env_prefix} -c conda-forge --override-channels -y \
    click \
    cmake \
    hypothesis \
    jinja2 \
    make \
    ncurses \
    ninja \
    numpy \
    scikit-build \
    tbb \
    wheel) || return 1

}


################################################################################
# Command Execution Functions
################################################################################
# These functions provide utilities for executing commands, testing environment
# configurations, and handling common operations with error checking and retries

# Function to determine the appropriate conda environment prefix format
# This handles both named environments and environments specified by path
env_name_or_prefix () {
  # Parameter: Environment name or path
  local env=$1

  # Check if the environment is specified as a path (starts with '/')
  if [[ ${env} == /* ]]; then
    # If the input string is a PATH (i.e. starts with '/'), then determine the
    # Conda environment by directory prefix using the -p flag
    echo "-p ${env}";
  else
    # Otherwise, determine the Conda environment by name using the -n flag
    echo "-n ${env}";
  fi
}

# Function to execute a command with multiple retry attempts
# This is useful for commands that might fail due to network issues or race conditions
exec_with_retries () {
  # Parameters:
  # $1: Maximum number of retry attempts
  # $2+: The command to execute with its arguments
  local max_retries="$1"
  local delay_secs=2  # Delay between retry attempts in seconds
  local retcode=0     # Return code of the command

  # Try the command up to max_retries+1 times (initial attempt + retries)
  # shellcheck disable=SC2086
  for i in $(seq 0 ${max_retries}); do
    # Print the attempt number and command being executed
    # shellcheck disable=SC2145
    echo "[EXEC] [ATTEMPT ${i}/${max_retries}]    + ${@:2}"

    # Execute the command
    if "${@:2}"; then
      # Command succeeded, set return code to 0 and exit the loop
      local retcode=0
      break
    else
      # Command failed, store the return code
      local retcode=$?
      echo "[EXEC] [ATTEMPT ${i}/${max_retries}] Command attempt failed."
      echo ""

      # If this wasn't the last attempt, wait before retrying
      if [ "$i" -ne "$max_retries" ]; then
        sleep $delay_secs
      fi
    fi
  done

  # If the command ultimately failed after all attempts, print a message
  if [ $retcode -ne 0 ]; then
    echo "[EXEC] The command has failed after ${max_retries} + 1 attempts; aborting."
  fi

  # Return the final status code
  return $retcode
}

# Function to test network connectivity
# This verifies that the system can connect to external resources
test_network_connection () {
  # Try to connect to pypi.org with retries
  exec_with_retries 3 wget -q --timeout 1 pypi.org -O /dev/null
  local exit_status=$?

  # Reference: https://man7.org/linux/man-pages/man1/wget.1.html
  # Check if the connection was successful (exit status 0)
  if [ $exit_status == 0 ]; then
    echo "[CHECK] Network does not appear to be blocked."
  else
    # Connection failed, provide diagnostic information and suggestions
    echo "[CHECK] Network check exit status: ${exit_status}"
    echo "[CHECK] Network appears to be blocked or suffering from poor connection."
    return 1
  fi
}

# Function to test if a binary is available in the PATH of a conda environment
# This is used to verify that required executables are properly installed
test_binpath () {
  # Parameters:
  # $1: Name of the conda environment
  # $2: Name of the binary to check for
  local env_name="$1"
  local bin_name="$2"

  # Validate required parameters
  if [ "$bin_name" == "" ]; then
    echo "Usage: ${FUNCNAME[0]} ENV_NAME BIN_NAME"
    echo "Example(s):"
    echo "    ${FUNCNAME[0]} build_env nvcc"
    return 1
  fi

  # Get the environment prefix format (-n or -p)
  # shellcheck disable=SC2155
  local env_prefix=$(env_name_or_prefix "${env_name}")

  # Check if the binary exists in the PATH of the conda environment
  # shellcheck disable=SC2086
  if conda run ${env_prefix} which "${bin_name}"; then
    echo "[CHECK] Binary ${bin_name} found in PATH"
  else
    echo "[CHECK] Binary ${bin_name} not found in PATH!"
    return 1
  fi
}

# Function to print and execute a command
# This provides visibility into what commands are being run
print_exec () {
  # Print the command that will be executed
  echo "+ $*"
  echo ""

  # Execute the command and capture its return code
  if eval "$*"; then
    local retcode=0
  else
    local retcode=$?
  fi

  echo ""
  # Return the command's exit status
  return $retcode
}

# Function to test if a file exists in a conda environment
# This is used to verify that required files are properly installed
test_filepath () {
  # Parameters:
  # $1: Name of the conda environment
  # $2: Name of the file to check for
  local env_name="$1"
  local file_name="$2"

  # Validate required parameters
  if [ "$file_name" == "" ]; then
    echo "Usage: ${FUNCNAME[0]} ENV_NAME FILE_NAME"
    echo "Example(s):"
    echo "    ${FUNCNAME[0]} build_env cuda_runtime.h"
    return 1
  fi

  # Get the environment prefix format (-n or -p)
  # shellcheck disable=SC2155
  local env_prefix=$(env_name_or_prefix "${env_name}")

  # Get the conda environment prefix path
  # shellcheck disable=SC2155,SC2086
  local conda_prefix=$(conda run ${env_prefix} printenv CONDA_PREFIX)

  # Search for the file in the conda environment (both regular files and symlinks)
  # shellcheck disable=SC2155
  local file_path=$(find "${conda_prefix}" -type f -name "${file_name}")
  # shellcheck disable=SC2155
  local link_path=$(find "${conda_prefix}" -type l -name "${file_name}")

  # Check if the file was found and report its location
  if [ "${file_path}" != "" ]; then
    echo "[CHECK] ${file_name} found in CONDA_PREFIX PATH (file): ${file_path}"
  elif [ "${link_path}" != "" ]; then
    echo "[CHECK] ${file_name} found in CONDA_PREFIX PATH (symbolic link): ${link_path}"
  else
    echo "[CHECK] ${file_name} not found in CONDA_PREFIX PATH!"
    return 1
  fi
}

# Function to test if a Python package can be imported in a conda environment
# This verifies that required Python packages are properly installed
test_python_import_package () {
  # Parameters:
  # $1: Name of the conda environment
  # $2: Name of the Python package to import
  local env_name="$1"
  local python_import="$2"

  # Validate required parameters
  if [ "$python_import" == "" ]; then
    echo "Usage: ${FUNCNAME[0]} ENV_NAME PYTHON_IMPORT"
    echo "Example(s):"
    echo "    ${FUNCNAME[0]} build_env numpy"
    return 1
  fi

  # Get the environment prefix format (-n or -p)
  # shellcheck disable=SC2155
  local env_prefix=$(env_name_or_prefix "${env_name}")

  # Try to import the Python package in the conda environment
  # shellcheck disable=SC2086
  if conda run ${env_prefix} python -c "import ${python_import}"; then
    echo "[CHECK] Python (sub-)package '${python_import}' found ..."
  else
    echo "[CHECK] Python (sub-)package '${python_import}' was not found! Please check if the Python sources have been packaged correctly."
    return 1
  fi
}


# Function to generate a standalone executable from a Python script using PyInstaller
# This creates a self-contained executable that includes all necessary dependencies
generate_standalone_executable () {
  # Install PyInstaller if not already installed
  echo "[SETUP] Installing PyInstaller for creating standalone executable..."
  pip install pyinstaller

  # Define the path to the benchmark script that will be converted to an executable
  echo "[SETUP] Setting up paths for PyInstaller..."
  SCRIPT_PATH="bench/tbe/tbe_inference_benchmark.py"

  # Define the output directory where the executable will be placed
  DIST_DIR="${BENCHMARKS_DIR}"

  # Find all shared libraries (.so files) that need to be included in the executable
  echo "[SETUP] Finding shared libraries to include in the executable..."
  # shellcheck disable=SC2086
  SHARED_LIBS=$(find ./_skbuild/linux-${MACHINE_NAME_LC}-3.13 -name "*.so" -printf "%p:fbgemm_gpu\n")

  # Build the standalone executable using PyInstaller
  # --onefile: Create a single executable file
  # --distpath: Specify the output directory
  # --add-binary: Include binary files (shared libraries) in the executable
  echo "[BUILD] Building standalone executable with PyInstaller..."
  # shellcheck disable=SC2046,SC2086
  pyinstaller --onefile --distpath $DIST_DIR $SCRIPT_PATH $(echo $SHARED_LIBS | xargs -n 1 echo --add-binary)

  # Notify the user that the build is complete
  echo "[SUCCESS] Build complete. Executable is located in the $DIST_DIR directory."
}


################################################################################
# Setup Functions
################################################################################
# These functions handle the setup of directories, environments, and dependencies
# required for building the embedding benchmark tools

# Function to set up directories for the build process
# This creates necessary directories and prepares the build environment
setup_directories() {
  echo "[SETUP] Setting up directories for the build process..."

  # Create the benchmarks directory if it doesn't exist
  # This is where the final executable will be placed
  echo "[SETUP] Creating benchmarks directory..."
  # shellcheck disable=SC2086
  mkdir -p ${BENCHMARKS_DIR}

  # Remove any existing build directory to ensure a clean build environment
  # This prevents issues with previous failed builds
  echo "[SETUP] Removing any existing build directory..."
  rm -rf build

  # Create a new build directory for the current build process
  # This is where all intermediate build files will be stored
  echo "[SETUP] Creating new build directory..."
  mkdir -p build

  # Change the current directory to the build directory
  # pushd saves the current directory on a stack so we can return to it later with popd
  echo "[SETUP] Changing to build directory..."
  pushd build || exist

  echo "[SETUP] Directory setup complete."
}

# Function to set up Miniconda and create a Conda environment
# This installs Miniconda and creates a Python environment with the specified version
setup_conda_environment() {
  echo "[SETUP] Setting up Conda environment..."

  # Set up Miniconda in the specified prefix directory
  # This installs the Conda package manager
  echo "[SETUP] Installing Miniconda..."
  # shellcheck disable=SC2086
  setup_miniconda $MINICONDA_PREFIX

  # Create a new Conda environment with the specified Python version
  # This environment will be used for building and running the benchmark tools
  echo "[SETUP] Creating Conda environment with Python ${PYTHON_VERSION}..."
  create_conda_environment $BUILD_ENV $PYTHON_VERSION

  # Activate the newly created Conda environment
  # This makes the environment's packages and executables available in the current shell
  echo "[SETUP] Activating Conda environment..."
  # shellcheck disable=SC2086
  eval "$(${MINICONDA_PREFIX}/bin/conda shell.bash hook)"
  conda activate ${BUILD_ENV}

  echo "[SETUP] Conda environment setup complete."
}

# Function to install necessary build tools and compilers
# This installs the C/C++ compiler and other build tools in the Conda environment
install_tools_and_compilers() {
  echo "[SETUP] Installing build tools and compilers..."

  # Install the C/C++ compiler in the Conda environment
  # This is needed for compiling C++ extensions in FBGEMM
  echo "[SETUP] Installing C/C++ compiler..."
  install_cxx_compiler $BUILD_ENV

  # Install additional build tools required for the project
  # This includes CMake, Ninja, and other build dependencies
  echo "[SETUP] Installing additional build tools..."
  install_build_tools $BUILD_ENV

  echo "[SETUP] Build tools and compilers installation complete."
}

# Function to install PyTorch via PIP
# This installs the CPU variant of PyTorch in the Conda environment
install_pytorch() {
  echo "[SETUP] Installing PyTorch ${PYTORCH_VERSION}..."

  # Check the network connection before proceeding
  # This ensures we can download PyTorch packages
  echo "[SETUP] Checking network connection..."
  test_network_connection || return 1

  # Install the CPU variant of PyTorch using PIP
  # We use the pre-release version and specify the CPU-only variant
  echo "[SETUP] Installing PyTorch CPU variant..."
  conda run -n $BUILD_ENV pip install --pre torch==${PYTORCH_VERSION} --index-url https://download.pytorch.org/whl/cpu/

  # Test if the PyTorch package loads correctly by importing a submodule
  # This verifies that PyTorch is properly installed
  echo "[CHECK] Testing PyTorch installation..."
  python -c "import torch.distributed"

  # Print the installed PyTorch version to verify the correct version is installed
  echo "[CHECK] Verifying PyTorch version..."
  python -c "import torch; print(torch.__version__)"

  echo "[SETUP] PyTorch installation complete."
}

# Function to clone the FBGEMM repository
# This downloads the FBGEMM source code from GitHub
clone_fbgemm_repo() {
  echo "[SETUP] Cloning FBGEMM repository version ${FBGEMM_VERSION}..."

  # Check the network connection before proceeding
  # This ensures we can download from GitHub
  echo "[SETUP] Checking network connection..."
  test_network_connection || return 1

  # Clone the FBGEMM repository along with its submodules
  # We specify the version tag to ensure we get the correct version
  echo "[SETUP] Cloning repository with submodules..."
  git clone --recursive -b ${FBGEMM_VERSION} https://github.com/pytorch/FBGEMM.git fbgemm_${FBGEMM_VERSION}

  # Disable the postbuild script to prevent race conditions during linking
  # This is a workaround for a known issue in the build process
  echo "[SETUP] Disabling postbuild script..."
  echo "#!/bin/bash" > fbgemm_${FBGEMM_VERSION}/.github/scripts/fbgemm_gpu_postbuild.bash

  # Change the current directory to the FBGEMM GPU directory
  pushd fbgemm_${FBGEMM_VERSION}/fbgemm_gpu || exist

  echo "[SETUP] FBGEMM repository setup complete."
}

# Function to install FBGEMM requirements and build the library
# This builds the FBGEMM library and creates a standalone executable
install_fbgemm() {
  echo "[BUILD] Installing FBGEMM requirements and building the library..."

  # Install the required Python packages for FBGEMM
  # These are specified in the requirements.txt file
  echo "[BUILD] Installing Python dependencies..."
  pip install -r requirements.txt

  # Set the package name based on the build variant
  # We're building the CPU variant of FBGEMM GPU
  echo "[BUILD] Setting build configuration variables..."
  export package_name=fbgemm_gpu_cpu

  # Set the package channel to 'test'
  # This is used for package distribution
  export package_channel=test

  # Set the Python tag based on the Python version
  # This ensures compatibility with Python 3.13
  export python_tag=py313

  # Determine the processor architecture
  # This is used for platform-specific builds
  # shellcheck disable=SC2155
  export ARCH=$(uname -m)

  # Set the Python platform name for Linux
  # This specifies the minimum glibc version required
  export python_plat_name="manylinux_2_28_${ARCH}"


  # Extract the number of CPU cores on the system
  # shellcheck disable=SC2155
  local core=$(lscpu | grep "Core(s)" | awk '{print $NF}') && echo "core = ${core}" || echo "core not found"
  # shellcheck disable=SC2155
  local sockets=$(lscpu | grep "Socket(s)" | awk '{print $NF}') && echo "sockets = ${sockets}" || echo "sockets not found"
  local re='^[0-9]+$'

  local run_multicore=""
  if [[ $core =~ $re && $sockets =~ $re ]]; then
    local n_core=$((core * sockets))
    run_multicore="-j ${n_core}"
  fi

  # Build and install the FBGEMM library into the Conda environment
  # We specify the CPU variant to build without CUDA support
  echo "[BUILD] Building and installing FBGEMM..."
  # shellcheck disable=SC2086
  print_exec conda run -n $BUILD_ENV python setup.py ${run_multicore} install --package_variant=cpu

  # Generate a standalone executable for the project
  # This creates a self-contained executable that can be run without Python
  echo "[BUILD] Generating standalone executable..."
  generate_standalone_executable

  # Return to the previous directory (outside of fbgemm_gpu)
  # This restores the directory we were in before entering fbgemm_gpu
  echo "[BUILD] Returning to previous directory..."
  popd || exist

  echo "[BUILD] FBGEMM installation complete."
}

# Function to clean up build directory
cleanup() {
  echo "[CLEANUP] Performing cleanup operations..."

  # This restores the directory we were in before entering the build directory
  echo "[CLEANUP] Returning to original directory..."
  popd || exist

  # Remove the build directory to clean up after the build process
  # This saves disk space by removing intermediate build files
  echo "[CLEANUP] Removing build directory..."
  rm -rf build

  echo "[CLEANUP] Cleanup complete."
}


################################################################################
# Main Functions
################################################################################

# Main function to orchestrate the setup and installation process
main() {
  echo "################################################################################"
  echo "# Starting EmbeddingBench Installation"
  echo "# $(date)"
  echo "################################################################################"

  # Set up directories for the build process
  echo "[MAIN] Setting up directories..."
  setup_directories

  # Set up Miniconda and create a Conda environment
  echo "[MAIN] Setting up Conda environment..."
  setup_conda_environment

  # Install necessary build tools and compilers
  echo "[MAIN] Installing build tools and compilers..."
  install_tools_and_compilers

  # Install PyTorch
  echo "[MAIN] Installing PyTorch..."
  install_pytorch

  # Clone the FBGEMM repository
  echo "[MAIN] Cloning FBGEMM repository..."
  clone_fbgemm_repo

  # Install FBGEMM requirements and build the library
  echo "[MAIN] Building FBGEMM..."
  install_fbgemm

  # Clean up temporary files and directories
  echo "[MAIN] Cleaning up..."
  cleanup

  # Output success message indicating the benchmark installation is complete
  echo "################################################################################"
  echo "# Installation Complete"
  echo "# tbe_inference_benchmark installed into ./benchmarks"
  echo "# $(date)"
  echo "################################################################################"
}


main
