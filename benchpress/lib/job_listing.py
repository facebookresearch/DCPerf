#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

from typing import List, Mapping, Optional

import tabulate


JOB_TAG_GROUP = ["scope", "component"]

TABLE_HEADERS = ["Job", "Description"]
TABLE_HEADERS_TAG = ["Job", "Tags", "Description"]


def formalize_tags(configs) -> Mapping[str, List[str]]:
    tags = {k: [] for k in JOB_TAG_GROUP}
    for config in configs:
        for k in tags:
            tags[k].extend(config.get("tags", {}).get(k, []))
    return tags


def get_tag_str(tags: Mapping[str, List[str]]) -> str:
    all_tags = []
    for k in tags:
        all_tags += sorted(tags[k])
    return ",".join(all_tags)


def create_job_listing(
    jobs: List[Mapping],
    table_format: str,
    group_key: Optional[str] = None,
) -> str:
    headers = TABLE_HEADERS_TAG
    sections = {"default": []}
    if group_key == "scope" or group_key == "component":
        headers = TABLE_HEADERS
        sections = {}

    max_width = [0 for _ in headers]
    for job in jobs:
        if group_key == "scope" or group_key == "component":
            row = [job["name"], job["description"]]
            if len(job["tags"][group_key]) > 0:
                for tag in job["tags"][group_key]:
                    if tag not in sections:
                        sections[tag] = []
                    sections[tag].append(row)
            else:
                if "other" not in sections:
                    sections["other"] = []
                sections["other"].append(row)
            max_width = [max(max_width[i], len(row[i])) for i in range(len(row))]
        else:
            row = [job["name"], get_tag_str(job["tags"]), job["description"]]
            sections["default"].append(row)

    table = []
    for tag, rows in sections.items():
        if tag != "default":
            splitter = "-" * int((max_width[0] - len(tag)) / 2 - 1)
            table.append([f"{splitter} {tag} {splitter}", "-" * max_width[1]])
        table.extend(rows)
    return tabulate.tabulate(table, headers=headers, tablefmt=table_format)
