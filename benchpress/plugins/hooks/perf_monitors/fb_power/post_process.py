#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import json
import logging
import os
import re

import pandas as pd

from .constants import VR_EFFICIENCY

logger = logging.getLogger(__name__)

CONFIGS_DIR = os.path.join(os.path.dirname(__file__), "configs")


class PowerPostProcessor:
    """Offline post-processing engine for fb-power.csv files.

    Reads raw sensor data + platform-specific JSON config to produce
    a summary CSV with decomposed power components.
    """

    def __init__(self, config_path: str):
        with open(config_path) as f:
            self.config = json.load(f)
        self.vr_efficiency = self.config.get("vr_efficiency", VR_EFFICIENCY)
        self.components = self.config.get("components", {})
        self.derived_metrics = self.config.get("derived_metrics", {})

    def process(self, raw_csv_path: str) -> pd.DataFrame:
        """Process raw fb-power.csv into a summary DataFrame.

        Args:
            raw_csv_path: Path to raw fb-power.csv from online collection.

        Returns:
            DataFrame with component power columns (e.g., server_power_w,
            cpu_power_w, memory_power_w, etc.)
        """
        df = pd.read_csv(raw_csv_path)
        available_columns = [c for c in df.columns if c not in ("index", "timestamp")]
        result = df[["index", "timestamp"]].copy()

        for comp_name, comp_config in self.components.items():
            matched_cols = self._match_sensors(comp_config, available_columns)
            output_col = f"{comp_name}_power_w"

            if not matched_cols:
                logger.warning(
                    f"No sensors matched for component '{comp_name}'. "
                    f"Pattern: {comp_config.get('sensor_pattern', comp_config.get('sensors', []))}"
                )
                result[output_col] = 0.0
                continue

            aggregation = comp_config.get("aggregation", "sum")
            if aggregation == "sum":
                result[output_col] = df[matched_cols].sum(axis=1)
            elif aggregation == "average":
                result[output_col] = df[matched_cols].mean(axis=1)
            elif aggregation == "max":
                result[output_col] = df[matched_cols].max(axis=1)
            else:
                logger.warning(f"Unknown aggregation '{aggregation}' for {comp_name}")
                result[output_col] = df[matched_cols].sum(axis=1)

            divisor = comp_config.get("divisor", 1)
            if divisor != 1:
                result[output_col] = result[output_col] / divisor

            if comp_config.get("apply_vr_loss", False):
                result[output_col] = result[output_col] * self.vr_efficiency

            logger.info(
                f"Component '{comp_name}': matched {len(matched_cols)} sensors "
                f"({', '.join(matched_cols[:3])}{'...' if len(matched_cols) > 3 else ''})"
            )

        for metric_name, formula in self.derived_metrics.items():
            try:
                result[metric_name] = self._evaluate_formula(formula, result, df)
            except Exception as e:
                logger.error(f"Failed to compute derived metric '{metric_name}': {e}")
                result[metric_name] = float("nan")

        return result

    def _match_sensors(self, comp_config: dict, available: list[str]) -> list[str]:
        """Match sensor columns by exact name or regex pattern."""
        matched = []

        for name in comp_config.get("sensors", []):
            if name in available:
                matched.append(name)
            else:
                for col in available:
                    if col.lower() == name.lower():
                        matched.append(col)
                        break

        pattern = comp_config.get("sensor_pattern")
        if pattern:
            regex = re.compile(pattern, re.IGNORECASE)
            for col in available:
                if regex.fullmatch(col) and col not in matched:
                    matched.append(col)

        return matched

    def _evaluate_formula(
        self, formula: str, result_df: pd.DataFrame, raw_df: pd.DataFrame
    ) -> pd.Series:
        """Evaluate a derived metric formula.

        Supports references to:
        - Component output columns (e.g., "cpu", "memory" → looks up "{name}_power_w")
        - Raw sensor columns from the original CSV
        - Constants: vr_efficiency
        """
        namespace = {}
        for col in result_df.columns:
            if col not in ("index", "timestamp"):
                namespace[col] = result_df[col]
                base_name = col.replace("_power_w", "")
                if base_name not in namespace:
                    namespace[base_name] = result_df[col]
        for col in raw_df.columns:
            if col not in ("index", "timestamp") and col not in namespace:
                namespace[col] = raw_df[col]
        namespace["vr_efficiency"] = self.vr_efficiency
        return eval(formula, {"__builtins__": {}}, namespace)

    def write_summary(self, df: pd.DataFrame, output_path: str):
        """Write the processed DataFrame to CSV."""
        df.to_csv(output_path, index=False)
        logger.info(f"Power summary written to {output_path}")

    @staticmethod
    def list_available_configs() -> list[str]:
        """List available platform config files."""
        if not os.path.isdir(CONFIGS_DIR):
            return []
        return sorted(f for f in os.listdir(CONFIGS_DIR) if f.endswith(".json"))

    @staticmethod
    def get_config_path(platform: str) -> str:
        """Get the config file path for a platform ID."""
        return os.path.join(CONFIGS_DIR, f"{platform}.json")
