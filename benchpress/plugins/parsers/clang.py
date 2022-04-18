import re

from benchpress.lib.parser import Parser


class ClangParser(Parser):
    def parse(self, stdout, stderr, returncode):
        metrics = {}
        input_indices = [1, 2, 3, 4]
        line_no = 0

        # Initialize one empty list per input file
        for i in input_indices:
            metrics[f"input{i}"] = []

        while line_no < len(stderr):
            line = stderr[line_no]
            # Find the wall time for all inputs
            for i in input_indices:
                input_name = f"input{i}"
                found_compiler_command = "clang++" in line and input_name in line
                while not found_compiler_command:
                    line_no += 1
                    if line_no >= len(stderr):
                        # No more compiler commands. Done with parsing.
                        return metrics
                    line = stderr[line_no]
                    found_compiler_command = "clang++" in line and input_name in line
                # Found compiler command
                while True:
                    line_no += 1
                    if line_no >= len(stderr):
                        raise Exception("Invalid clang benchmark log.")
                    line = stderr[line_no]
                    if "real" in line:
                        # Found line with wall time
                        wall_time = self.try_parse_time(line)
                        if wall_time == -1:
                            raise Exception(
                                "Failed to parse wall time from clang benchmark log"
                            )
                        metrics[input_name].append(wall_time)
                        break
            line_no += 1
        return metrics

    def try_parse_time(self, line: str) -> float:
        """
        Given a line in the bash time output format, e.g. "real    0m10.640s",
        parse the time and return it as seconds.
        """
        tokens = line.split()
        if len(tokens) < 2:
            return -1
        m = re.match(r"(.*)m(.*)s", tokens[1])
        if m is None:
            return -1
        try:
            minute = int(m.group(1))
            second = float(m.group(2))
        except ValueError:
            print("Failed to parse clang build time")
            return -1
        return minute * 60 + second
