# Copyright (c) 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.
import logging
import logging.handlers
import os


class ConditionalFormatter(logging.Formatter):
    def format(self, record):
        if hasattr(record, "raw") and record.raw:
            return record.getMessage()
        else:
            return logging.Formatter.format(self, record)


handler = logging.handlers.WatchedFileHandler("benchpress.log")
formatter = ConditionalFormatter(
    "[%(asctime)s] %(name)-12s %(levelname)-8s: %(message)s"
)
handler.setFormatter(formatter)

stream_handler = logging.StreamHandler()
stream_handler.setLevel(logging.WARNING)


def create_logger():
    root = logging.getLogger()
    root.setLevel(os.environ.get("LOGLEVEL", "INFO"))
    root.addHandler(handler)
    root.addHandler(stream_handler)
    return root
