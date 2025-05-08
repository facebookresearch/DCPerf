#!/usr/bin/python3

# pyre-unsafe
from . import utils


if __name__ == "__main__":
    parser = utils.init_parser()
    args = parser.parse_args()
    utils.process_metrics(args, dump_overall_metrics=utils.dump_overall_metrics)
