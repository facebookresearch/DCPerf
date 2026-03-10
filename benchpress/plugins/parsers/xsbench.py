# (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.


import re
from typing import Dict, List, Optional

from benchpress.lib.parser import Parser


class XSBenchParser(Parser):
    """
    Parser for the XSBench benchmark.
    Extracts metrics from the INPUT SUMMARY and RESULTS sections of the output.
    """

    def parse(
        self, stdout: List[str], stderr: List[str], returncode: int
    ) -> Dict[str, object]:
        """
        Parse XSBench output and extract metrics from INPUT SUMMARY and RESULTS sections.

        Args:
            stdout: List of stdout lines from the benchmark
            stderr: List of stderr lines from the benchmark
            returncode: Return code from the benchmark execution

        Returns:
            Dictionary containing parsed metrics from both sections
        """
        results: Dict[str, object] = {}

        content = "\n".join(stdout)

        input_summary = self._extract_section(content, "INPUT SUMMARY")
        if input_summary:
            input_metrics = self._parse_key_values(input_summary)
            results["input_summary"] = input_metrics
        else:
            print("Cannot find summary ...")
            print("stdout:", stdout)
            print("stderr:", stderr)

        results_section = self._extract_section(content, "RESULTS")
        if results_section:
            results_metrics = self._parse_key_values(results_section)
            results["results"] = results_metrics

        results["returncode"] = returncode

        return results

    def _extract_section(self, content: str, section_name: str) -> Optional[str]:
        """
        Extract a named section from the log content.

        Sections are delimited by lines of '=' characters with the section name
        appearing between them.

        Args:
            content: Full log file content
            section_name: Name of the section to extract (e.g., "RESULTS", "INPUT SUMMARY")

        Returns:
            The content of the section, or None if not found
        """
        separator = "=" * 80

        lines = content.split("\n")
        in_section = False
        section_lines = []
        skip_next_separator = False

        for line in lines:
            if section_name in line and separator not in line:
                in_section = True
                skip_next_separator = True
                continue
            if in_section:
                if line.strip() == separator:
                    if skip_next_separator:
                        skip_next_separator = False
                        continue
                    break
                if line.strip():
                    section_lines.append(line)

        return "\n".join(section_lines) if section_lines else None

    def _parse_key_values(self, section: str) -> Dict[str, object]:
        """
        Parse key-value pairs from a section.

        Args:
            section: The content of a section

        Returns:
            Dictionary of parsed metrics
        """
        metrics: Dict[str, object] = {}

        for line in section.split("\n"):
            if ":" in line:
                key, value = line.split(":", 1)
                key = key.strip().lower().replace(" ", "_").replace("/", "_per_")
                key = key.replace("-", "_").replace("(", "").replace(")", "")
                value = value.strip()

                parsed_value = self._parse_value(value)
                metrics[key] = parsed_value

        return metrics

    def _parse_value(self, value: str) -> object:
        """
        Parse a value string into the appropriate type.

        Args:
            value: String value to parse

        Returns:
            Parsed value (int, float, or string)
        """
        value_clean = value.replace(",", "")

        if match := re.match(r"^([\d.]+)\s*seconds?$", value_clean):
            return float(match.group(1))

        try:
            return int(value_clean)
        except ValueError:
            pass

        try:
            return float(value_clean)
        except ValueError:
            pass

        return value


if __name__ == "__main__":
    parser = XSBenchParser()
    # read from log.txt, serve it as stdout
    with open("log.txt", "r") as f:
        stdout = f.readlines()
    metrics = parser.parse(stdout, [], 0)
    print(metrics)
