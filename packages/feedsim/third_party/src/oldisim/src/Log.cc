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

#include "oldisim/Log.h"

#include <stdio.h>
#include <stdarg.h>

log_level_t log_level = INFO;

void log_file_line(log_level_t level, const char *file, int line,
                   const char *format, ...) {
  if (level < log_level) {
    return;
  }

  va_list args;
  char new_format[512];

  snprintf(new_format, sizeof(new_format), "%s(%d): %s\n", file, line, format);

  va_start(args, format);
  vfprintf(stderr, new_format, args);
  va_end(args);
}
