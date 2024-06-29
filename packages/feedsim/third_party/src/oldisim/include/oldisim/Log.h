// Copyright 2015 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef OLDISIM_LOG_H
#define OLDISIM_LOG_H

#include <stdlib.h>

enum log_level_t { DEBUG, VERBOSE, INFO, WARN, QUIET };
extern log_level_t log_level;

void log_file_line(log_level_t level, const char* file, int line,
                   const char* format, ...);
#define L(level, args...) log_file_line(level, __FILE__, __LINE__, args)

#define D(args...) L(DEBUG, args)
#define V(args...) L(VERBOSE, args)
#define I(args...) L(INFO, args)
#define W(args...) L(WARN, args)

#define DIE(args...) \
  do {               \
    W(args);         \
    exit(-1);        \
  } while (0)

#define NOLOG(x)                 \
  do {                           \
    log_level_t old = log_level; \
    log_level = QUIET;           \
    (x);                         \
    log_level = old;             \
  } while (0)

#endif  // OLDISIM_LOG_H
