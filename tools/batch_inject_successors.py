#!/usr/bin/env python3
"""
batch_inject_successors.py [root_dir]

Recursively walks root_dir (default: cases/) for *.h files, and runs
inject_successors.process_file on each one that has the expected
`subgraph` struct + `test_graph[]` shape. Each match is rewritten IN PLACE:
the injected suc_cnt/suc_idx/successors/total_pre_cnt fields and the
total_task_id/total_type/total_duration arrays are written directly back
into the input file itself (not a sibling <name>_suc.h). No input
filenames need to be given.

Files that don't match the expected shape are skipped and reported, not
silently ignored. Files whose `subgraph` struct already carries a `suc_cnt`
field are treated as already-processed and skipped, so re-running this is
idempotent. Leftover `*_suc.h` files from the old v1-style tool are also
skipped.
"""
import sys
from pathlib import Path

from inject_successors import parse_struct_fields, process_file


def already_injected(text):
    """True if the file's `subgraph` struct already has the injected fields."""
    try:
        _, fields, _ = parse_struct_fields(text)
    except ValueError:
        return False
    return "suc_cnt" in fields


def main():
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("cases")

    processed = []
    skipped = []
    for h_file in sorted(root.rglob("*.h")):
        if h_file.stem.endswith("_suc"):
            continue
        try:
            if already_injected(h_file.read_text()):
                raise ValueError("already injected (suc_cnt field present)")
            out_path, total_local, total_edges, n_groups, total_task_cnt = process_file(
                h_file, output_path=h_file
            )
            processed.append(h_file)
            print(f"OK   {h_file}: {total_local}/{total_edges} edges, {n_groups} group(s), total_task_cnt={total_task_cnt}")
        except Exception as e:
            skipped.append((h_file, e))
            print(f"SKIP {h_file}: {e}", file=sys.stderr)

    print(f"\n{len(processed)} file(s) processed, {len(skipped)} skipped")


if __name__ == "__main__":
    main()
