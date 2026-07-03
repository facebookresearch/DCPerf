# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Generate DLRM medium and large TorchScript models with random weights.

For benchmarking, trained weights aren't needed — the compute pattern
(embedding lookups, MLP forward passes, feature interactions) is identical
regardless of weight values. These models are architecture-compatible with
the existing dlrm_small.pt model used by FeedSim's DLRM inference path.

Usage:
    python3 generate_dlrm_models.py <output_dir>
"""

import os
import sys

import torch
from torch import nn


class DLRM(nn.Module):
    """Simplified DLRM model matching production inference patterns."""

    def __init__(
        self,
        emb_dim: int = 64,
        num_dense: int = 13,
        num_sparse: int = 26,
        max_emb_rows: int = 250000,
        bottom_mlp_dims: list = None,
        top_mlp_dims: list = None,
    ):
        super().__init__()
        if bottom_mlp_dims is None:
            bottom_mlp_dims = [256, 128, emb_dim]
        if top_mlp_dims is None:
            top_mlp_dims = [256, 128, 1]

        # Bottom MLP: dense features -> embedding dimension
        layers = []
        in_dim = num_dense
        for out_dim in bottom_mlp_dims:
            layers.append(nn.Linear(in_dim, out_dim))
            layers.append(nn.ReLU())
            in_dim = out_dim
        self.bottom_mlp = nn.Sequential(*layers)

        # Embedding tables for sparse features
        emb_sizes = [
            40000000,
            39060,
            17295,
            7424,
            20265,
            3,
            7122,
            1543,
            63,
            40000000,
            3067956,
            405282,
            10,
            2209,
            11938,
            155,
            4,
            976,
            14,
            40000000,
            40000000,
            40000000,
            590152,
            12973,
            108,
            36,
        ]
        # Use min(actual_size, max_emb_rows) to control model size
        self.embeddings = nn.ModuleList(
            [
                nn.EmbeddingBag(min(s, max_emb_rows), emb_dim, mode="sum")
                for s in emb_sizes[:num_sparse]
            ]
        )

        # Top MLP: interaction output -> prediction. ReLU only on hidden
        # layers; the final Linear feeds raw logits into sigmoid in forward().
        n = 1 + num_sparse  # bottom_mlp output + embedding outputs
        interaction_size = emb_dim + (n * (n - 1)) // 2
        top_layers = []
        in_dim = interaction_size
        for i, out_dim in enumerate(top_mlp_dims):
            top_layers.append(nn.Linear(in_dim, out_dim))
            if i < len(top_mlp_dims) - 1:
                top_layers.append(nn.ReLU())
            in_dim = out_dim
        self.top_mlp = nn.Sequential(*top_layers)

    def forward(self, dense: torch.Tensor, sparse: torch.Tensor) -> torch.Tensor:
        # Bottom MLP
        d = self.bottom_mlp(dense)

        # Embedding lookups
        embs = [emb(sparse[:, i].unsqueeze(1)) for i, emb in enumerate(self.embeddings)]

        # Feature interaction (dot product)
        combined = torch.cat([d.unsqueeze(1)] + [e.unsqueeze(1) for e in embs], dim=1)
        interact = torch.bmm(combined, combined.transpose(1, 2))
        n = combined.size(1)
        idx = torch.triu_indices(n, n, offset=1)
        flat = interact[:, idx[0], idx[1]]

        # Top MLP
        x = torch.cat([d, flat], dim=1)
        return torch.sigmoid(self.top_mlp(x))


def generate_model(output_path: str, **kwargs):
    """Generate and save a TorchScript DLRM model."""
    model = DLRM(**kwargs)
    param_bytes = sum(p.numel() * p.element_size() for p in model.parameters())
    print(f"  Parameters: {sum(p.numel() for p in model.parameters()):,}")
    print(f"  Model size: {param_bytes / 1e6:.0f} MB")

    scripted = torch.jit.script(model)
    scripted.save(output_path)
    file_size = os.path.getsize(output_path)
    print(f"  Saved to: {output_path} ({file_size / 1e6:.0f} MB on disk)")


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <output_dir>")
        sys.exit(1)

    output_dir = sys.argv[1]
    os.makedirs(output_dir, exist_ok=True)

    # Medium model: larger embeddings (~500MB)
    medium_path = os.path.join(output_dir, "dlrm_medium.pt")
    if not os.path.exists(medium_path):
        print("Generating DLRM medium model...")
        generate_model(
            medium_path,
            emb_dim=64,
            max_emb_rows=500000,
            bottom_mlp_dims=[256, 128, 64],
            top_mlp_dims=[256, 128, 1],
        )
    else:
        print(f"[SKIPPED] {medium_path} already exists")

    # Large model: even larger embeddings (~1GB)
    large_path = os.path.join(output_dir, "dlrm_large.pt")
    if not os.path.exists(large_path):
        print("Generating DLRM large model...")
        generate_model(
            large_path,
            emb_dim=128,
            max_emb_rows=500000,
            bottom_mlp_dims=[512, 256, 128],
            top_mlp_dims=[512, 256, 1],
        )
    else:
        print(f"[SKIPPED] {large_path} already exists")


if __name__ == "__main__":
    main()
