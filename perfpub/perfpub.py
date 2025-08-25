#!/usr/bin/python3

# pyre-unsafe

import utils  # pyre-ignore[21]: This file is only used in open-source environments

if __name__ == "__main__":
    parser = utils.init_parser()
    args = parser.parse_args()
    utils.process_metrics(args, dump_overall_metrics=utils.dump_overall_metrics)
