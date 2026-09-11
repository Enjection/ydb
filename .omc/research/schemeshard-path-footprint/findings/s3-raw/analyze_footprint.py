#!/usr/bin/env python3
"""
Post-process S3 dynamic-oracle TSV dumps produced by the temporary
instrumentation in schemeshard_path.cpp / schemeshard__operation.cpp.

Line formats (tab-separated):
  TPath call:    <tag>\t<method>\t<pathStr>\t<isResolved 0/1>\t<pathId or empty>
  Propose status:<tag>\tSTATUS\t<statusName>

Tag format:
  REQUEST=<opType1>[+opType2...],txId=<txId>|part=<PartOpTypeName>#<counter>

This script:
  1. Parses all raw TSVs.
  2. Groups TPath-call lines by (requestOpTypes, partOpType).
  3. Normalizes concrete path strings into "shapes" (placeholders for
     dynamic segments like table/dir/stream names) - heuristic, based on
     known fixture naming patterns in the schemeshard UTs (MyRoot, generic
     names) plus a generic depth-based placeholder fallback.
  4. Aggregates: per part op type -> {shape -> count, methods used, resolved
     fraction}; per part op type -> Propose status distribution.
  5. Aggregates: per REQUEST op type -> distinct set of derived part op types
     observed (cross-check vs S2).
  6. Emits a summary as JSON + prints tables to stdout.
"""
import sys
import os
import re
import json
import glob
from collections import defaultdict, Counter

def normalize_path_shape(path):
    """Turn a concrete path string into a normalized 'shape'."""
    if not path:
        return "<empty>"
    parts = path.strip("/").split("/")
    out = []
    for i, p in enumerate(parts):
        if i == 0:
            # root/database segment - keep as-is (usually MyRoot in UTs)
            out.append(p)
            continue
        if p in (".backups", "collections", "indexImplTable", ".sys", ".sys_health"):
            out.append(p)
            continue
        # Heuristic: system/dot-prefixed names kept literal
        if p.startswith("."):
            out.append(p)
            continue
        # Otherwise replace with a positional placeholder classified by common suffixes
        low = p.lower()
        if "cdc" in low or "stream" in low:
            out.append("<stream>")
        elif "index" in low:
            out.append("<index>")
        elif "table" in low or re.match(r"^t\d*$", low) or re.match(r"^table\d*$", low):
            out.append("<table>")
        elif re.match(r"^dir\d*$", low):
            out.append("<dir>")
        else:
            out.append("<seg%d>" % i)
    return "/" + "/".join(out)


TAG_RE = re.compile(r"^REQUEST=(?P<req>[^,]*),txId=(?P<txid>[^|]*)\|part=(?P<part>[^#]*)#(?P<counter>\d+)$")


def parse_tag(tag):
    m = TAG_RE.match(tag)
    if not m:
        return None
    return {
        "req": m.group("req"),
        "txid": m.group("txid"),
        "part": m.group("part"),
        "counter": m.group("counter"),
    }


def main():
    raw_dir = sys.argv[1] if len(sys.argv) > 1 else "."
    out_json = sys.argv[2] if len(sys.argv) > 2 else "footprint_summary.json"

    files = sorted(glob.glob(os.path.join(raw_dir, "footprint.*.tsv")))
    if not files:
        print("No footprint.*.tsv files found in", raw_dir, file=sys.stderr)
        sys.exit(1)

    total_lines = 0
    parse_failures = 0

    # per part op type
    shapes_by_part = defaultdict(Counter)          # part -> shape -> count
    methods_by_part = defaultdict(Counter)         # part -> method -> count
    resolved_by_part = defaultdict(lambda: [0, 0]) # part -> [resolved, total]
    status_by_part = defaultdict(Counter)          # part -> status -> count

    # per request op type
    parts_by_request = defaultdict(Counter)        # requestOpTypes -> partOpType -> count

    example_paths_by_part_shape = defaultdict(dict)  # part -> shape -> example concrete path

    for fn in files:
        with open(fn, "r", errors="replace") as f:
            for line in f:
                line = line.rstrip("\n")
                if not line:
                    continue
                total_lines += 1
                cols = line.split("\t")
                if len(cols) < 3:
                    parse_failures += 1
                    continue
                tag = cols[0]
                method = cols[1]
                parsed = parse_tag(tag)
                if parsed is None:
                    parse_failures += 1
                    continue
                part = parsed["part"]
                req = parsed["req"]

                if method == "STATUS":
                    status = cols[2] if len(cols) > 2 else "?"
                    status_by_part[part][status] += 1
                    parts_by_request[req][part] += 1
                    continue

                # TPath call line: tag, method, pathStr, isResolved, pathId
                path_str = cols[2] if len(cols) > 2 else ""
                is_resolved = cols[3] if len(cols) > 3 else "0"

                shape = normalize_path_shape(path_str)
                shapes_by_part[part][shape] += 1
                methods_by_part[part][method] += 1
                resolved_by_part[part][1] += 1
                if is_resolved == "1":
                    resolved_by_part[part][0] += 1
                if shape not in example_paths_by_part_shape[part]:
                    example_paths_by_part_shape[part][shape] = path_str

    summary = {
        "files": files,
        "total_lines": total_lines,
        "parse_failures": parse_failures,
        "part_op_types_seen": sorted(set(list(shapes_by_part.keys()) + list(status_by_part.keys()))),
        "request_op_types_seen": sorted(parts_by_request.keys()),
        "per_part": {},
        "per_request": {},
    }

    for part in summary["part_op_types_seen"]:
        resolved, total = resolved_by_part.get(part, [0, 0])
        summary["per_part"][part] = {
            "shapes": [
                {"shape": s, "count": c, "example": example_paths_by_part_shape[part].get(s, "")}
                for s, c in shapes_by_part[part].most_common()
            ],
            "methods": dict(methods_by_part[part]),
            "resolved_fraction": (resolved / total) if total else None,
            "resolved_count": resolved,
            "total_path_calls": total,
            "status_distribution": dict(status_by_part[part]),
        }

    for req in summary["request_op_types_seen"]:
        summary["per_request"][req] = dict(parts_by_request[req])

    with open(out_json, "w") as f:
        json.dump(summary, f, indent=2, sort_keys=True)

    # Console report
    print(f"Parsed {total_lines} lines from {len(files)} files ({parse_failures} unparseable tag lines)")
    print(f"Distinct part op types observed: {len(summary['part_op_types_seen'])}")
    print(f"Distinct request op types observed: {len(summary['request_op_types_seen'])}")
    print()
    print("=== Per REQUEST op type -> derived PART op types ===")
    for req in sorted(summary["per_request"].keys()):
        parts = summary["per_request"][req]
        print(f"  {req}:")
        for p, c in sorted(parts.items(), key=lambda kv: -kv[1]):
            print(f"      {p}  (proposed x{c})")
    print()
    print("=== Per PART op type: path shapes touched ===")
    for part in sorted(summary["per_part"].keys()):
        info = summary["per_part"][part]
        print(f"  {part}: total_path_calls={info['total_path_calls']} resolved_fraction={info['resolved_fraction']}")
        print(f"      methods: {info['methods']}")
        print(f"      status_distribution: {info['status_distribution']}")
        for sh in info["shapes"][:15]:
            print(f"      shape={sh['shape']!r:50s} count={sh['count']:4d} example={sh['example']!r}")
    print(f"\nWrote {out_json}")


if __name__ == "__main__":
    main()
