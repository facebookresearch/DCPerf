// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ranking {

/**
 * SilesiaLoader - Loads the Silesia compression corpus via mmap.
 *
 * The Silesia corpus (http://sun.aei.polsl.pl/~sdeor/index.php?page=silesia)
 * is a 203MB dataset of 12 files representing diverse real-world data:
 * dickens, mozilla, mr, nci, ooffice, osdb, reymont, samba, sao, webster,
 * xml, x-ray.
 *
 * All files are mmapped at startup. Random snippets of configurable size
 * are served to callers. Thread-safe for concurrent reads (mmap data is
 * read-only; each thread uses its own RNG).
 */
class SilesiaLoader {
 public:
  struct MappedFile {
    std::string name;
    const uint8_t* data;
    size_t size;
    int fd;
  };

  SilesiaLoader() = default;

  ~SilesiaLoader() {
    for (auto& f : files_) {
      if (f.data) {
        munmap(const_cast<uint8_t*>(f.data), f.size);
      }
      if (f.fd >= 0) {
        close(f.fd);
      }
    }
  }

  // Non-copyable, movable
  SilesiaLoader(const SilesiaLoader&) = delete;
  SilesiaLoader& operator=(const SilesiaLoader&) = delete;
  SilesiaLoader(SilesiaLoader&&) = default;
  SilesiaLoader& operator=(SilesiaLoader&&) = default;

  /**
   * Load all files from the given directory via mmap.
   * Returns true if at least one file was loaded.
   */
  bool loadDirectory(const std::string& dir_path) {
    DIR* dir = opendir(dir_path.c_str());
    if (!dir) {
      std::cerr << "SilesiaLoader: cannot open directory: " << dir_path
                << std::endl;
      return false;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
      if (entry->d_name[0] == '.') continue;

      std::string filepath = dir_path + "/" + entry->d_name;
      struct stat st;
      if (stat(filepath.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        continue;
      }

      // Skip zero-length files — mmap of size 0 fails with EINVAL.
      if (st.st_size == 0) {
        continue;
      }

      int fd = open(filepath.c_str(), O_RDONLY);
      if (fd < 0) {
        std::cerr << "SilesiaLoader: cannot open file: " << filepath
                  << std::endl;
        continue;
      }

      void* mapped = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
      if (mapped == MAP_FAILED) {
        std::cerr << "SilesiaLoader: mmap failed for: " << filepath
                  << std::endl;
        close(fd);
        continue;
      }

      // getRandomSnippet performs random-offset reads across the map, so
      // MADV_RANDOM matches the actual access pattern better than
      // MADV_SEQUENTIAL (which would evict pages immediately after read).
      madvise(mapped, st.st_size, MADV_RANDOM);

      files_.push_back({entry->d_name,
                        static_cast<const uint8_t*>(mapped),
                        static_cast<size_t>(st.st_size),
                        fd});
      total_size_ += st.st_size;
    }
    closedir(dir);

    std::cout << "SilesiaLoader: loaded " << files_.size()
              << " files, total " << (total_size_ / (1024 * 1024))
              << " MB" << std::endl;
    return !files_.empty();
  }

  /**
   * Get a random snippet from the corpus.
   *
   * @param rng Random number generator (caller-owned, thread-local)
   * @param min_size Minimum snippet size in bytes
   * @param max_size Maximum snippet size in bytes
   * @param out_data Pointer to snippet data (valid as long as SilesiaLoader lives)
   * @param out_size Actual snippet size
   * @param out_filename Name of the source file
   */
  void getRandomSnippet(
      std::mt19937& rng,
      size_t min_size,
      size_t max_size,
      const uint8_t*& out_data,
      size_t& out_size,
      std::string& out_filename) const {
    // Guard: caller invoked getRandomSnippet without ever loading a file.
    if (files_.empty()) {
      out_data = nullptr;
      out_size = 0;
      out_filename.clear();
      return;
    }
    // Pick a random file
    std::uniform_int_distribution<size_t> file_dist(0, files_.size() - 1);
    const auto& file = files_[file_dist(rng)];

    // Pick a random snippet size
    size_t actual_max = std::min(max_size, file.size);
    size_t actual_min = std::min(min_size, actual_max);
    std::uniform_int_distribution<size_t> size_dist(actual_min, actual_max);
    size_t snippet_size = size_dist(rng);

    // Pick a random offset
    size_t max_offset = (file.size > snippet_size) ? (file.size - snippet_size) : 0;
    std::uniform_int_distribution<size_t> offset_dist(0, max_offset);
    size_t offset = offset_dist(rng);

    out_data = file.data + offset;
    out_size = snippet_size;
    out_filename = file.name;
  }

  size_t numFiles() const { return files_.size(); }
  size_t totalSize() const { return total_size_; }
  bool isLoaded() const { return !files_.empty(); }
  // Read-only accessor used by Silesia-backed response generators.
  const MappedFile& fileAt(size_t idx) const { return files_[idx]; }

 private:
  std::vector<MappedFile> files_;
  size_t total_size_ = 0;
};

} // namespace ranking
