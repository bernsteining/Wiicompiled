#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["xxhash>=3"]
# ///
"""Grow the bundled pipeline seed (runtime/assets/pipeline/initial_pipeline_cache.db) with the
pipeline configurations a running game learned (its Cache/pipeline_cache.db).

    pipeline_seed.py merge <seed.db> <learned.db> [<learned.db>...]

aurora compiles every seeded configuration at startup (its "prewarm"), so a configuration the seed
already holds no longer compiles in the middle of a race. That matters most in Safari, where a
compile stalls the whole page for up to a second.

Rows are matched by their configuration bytes, not their stored hash: a WebAssembly build stores
the low 32 bits of the hash (aurora's HashType is 32-bit there), native builds all 64. New rows get
the full hash, XXH3-64 of the configuration seeded with the pipeline type (aurora's xxh3_hash_s),
which both kinds of build accept. Only configuration versions the seed already uses are taken, so
rows from an older aurora are left out. A learned database in WAL mode needs its -wal file beside it.
"""

import argparse
import shutil
import sqlite3
import sys

import xxhash


def native_hash(row_type: int, config: bytes) -> int:
    value = xxhash.xxh3_64_intdigest(config, seed=row_type)
    return value - (1 << 64) if value >= 1 << 63 else value  # SQLite integers are signed


def merge(seed_path: str, learned_paths: list[str]) -> None:
    seed = sqlite3.connect(seed_path)
    rows = seed.execute("SELECT type, hash, config_version, config_size, config FROM pipeline_cache").fetchall()
    for row_type, stored, _, _, config in rows:
        if native_hash(row_type, bytes(config)) != stored:
            sys.exit(f"{seed_path}: a seed row's hash is not XXH3-64(config, seed=type); not touching it")
    versions = {(t, v) for t, _, v, _, _ in rows}
    known = {(t, bytes(c)) for t, _, _, _, c in rows}
    print(f"{seed_path}: {len(rows)} rows, configuration versions {sorted(versions)}")

    added = 0
    for learned_path in learned_paths:
        learned = sqlite3.connect(f"file:{learned_path}?mode=ro", uri=True)
        taken = skipped = 0
        for row_type, version, size, config, first_frame in learned.execute(
                "SELECT type, config_version, config_size, config, first_frame_used FROM pipeline_cache"):
            config = bytes(config)
            if (row_type, version) not in versions or size != len(config) or (row_type, config) in known:
                skipped += 1
                continue
            seed.execute("INSERT INTO pipeline_cache (type, hash, config_version, config_size, config, first_frame_used) "
                         "VALUES (?, ?, ?, ?, ?, ?)",
                         (row_type, native_hash(row_type, config), version, size, config, first_frame))
            known.add((row_type, config))
            taken += 1
        learned.close()
        print(f"{learned_path}: {taken} new configurations, {skipped} already in the seed or unusable")
        added += taken
    seed.commit()
    seed.execute("VACUUM")
    total = seed.execute("SELECT COUNT(*) FROM pipeline_cache").fetchone()[0]
    seed.close()
    print(f"{seed_path}: {added} rows added, {total} in total")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    merge_parser = sub.add_parser("merge", help="add learned configurations to a seed database, in place")
    merge_parser.add_argument("seed")
    merge_parser.add_argument("learned", nargs="+")
    merge_parser.add_argument("--backup", action="store_true", help="keep <seed>.bak")
    args = parser.parse_args()
    if args.backup:
        shutil.copyfile(args.seed, args.seed + ".bak")
    merge(args.seed, args.learned)


if __name__ == "__main__":
    main()
