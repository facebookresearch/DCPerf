# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


from __future__ import annotations


_ARGUMENTS_WITH_MEANINGFUL_ZERO: frozenset[str] = frozenset({"--timeout-buffer"})


def append_server_argument(
    command: list[str], argument_name: str, argument_value: object
) -> None:
    if isinstance(argument_value, bool):
        if argument_value:
            command.append(argument_name)
        return
    if argument_value is None or argument_value == "":
        return
    if argument_value == 0 and argument_name not in _ARGUMENTS_WITH_MEANINGFUL_ZERO:
        return
    command.extend([argument_name, str(argument_value)])
