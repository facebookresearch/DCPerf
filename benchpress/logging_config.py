# Copyright (c) 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.
import logging
import logging.handlers
import os


handler = logging.handlers.WatchedFileHandler("benchpress.log")
formatter = logging.Formatter("[%(asctime)s] %(name)-12s %(levelname)-8s: %(message)s")
handler.setFormatter(formatter)


def create_logger():
    root = logging.getLogger()
    root.setLevel(os.environ.get("LOGLEVEL", "INFO"))
    root.addHandler(handler)
    root.addHandler(logging.StreamHandler())
    return root
