# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
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


def reconfigure_log_path(artifacts_dir: str):
    """Reconfigure the file handler to write benchpress.log to the artifacts directory."""
    global handler
    new_path = os.path.join(artifacts_dir, "benchpress.log")
    new_handler = logging.handlers.WatchedFileHandler(new_path)
    new_handler.setFormatter(formatter)
    root = logging.getLogger()
    root.removeHandler(handler)
    root.addHandler(new_handler)
    handler = new_handler
