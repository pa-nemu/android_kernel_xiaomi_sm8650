#!/usr/bin/env python3
"""Compare a commit, or find the local tag with the fewest changed text lines."""

import argparse
import re
from concurrent.futures import ThreadPoolExecutor
import subprocess
import sys


def run_git(*args):
    result = subprocess.run(
        ["git", *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, encoding="utf-8", errors="replace",
    )
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or "Git command failed")
    return result.stdout


def get_tags(pattern):
    return run_git("tag", "--list", "--", pattern).splitlines()


def get_total_changes(tag):
    ref = tag if re.fullmatch(r"[0-9a-fA-F]{7,40}", tag) else "refs/tags/" + tag
    commit = run_git("rev-parse", "--verify", "--end-of-options",
                     ref + "^{commit}").strip()
    # Numstat handles zero changes, additions-only and deletions-only diffs.
    # Binary files have no line count and are excluded from this metric.
    stats = run_git("diff", "--no-ext-diff", "--no-textconv", "--numstat",
                    "-z", commit, "--")
    total = 0
    records = iter(stats.split("\0"))
    for record in records:
        if not record:
            continue
        added, deleted, path = record.split("\t", 2)
        if not path:  # With -z, renamed files have two extra path records.
            next(records)
            next(records)
        if added != "-" and deleted != "-":
            total += int(added) + int(deleted)
    return total


def compare_tag(tag):
    return tag, get_total_changes(tag)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-j", dest="jobs", default=1, type=int, metavar="N",
                        help="number of jobs to run at once")
    parser.add_argument("-s", dest="silent", action="store_true",
                        help="reduce the verbosity of the output")
    parser.add_argument("tag_name", metavar="<Tag Pattern or Commit SHA>",
                        help="local tag pattern (can contain wildcards), or commit SHA")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("-j must be at least 1")

    try:
        run_git("rev-parse", "--show-toplevel")
        tags = get_tags(args.tag_name)
        is_commit = False
        if not tags and re.fullmatch(r"[0-9a-fA-F]{7,40}", args.tag_name):
            commit = run_git("rev-parse", "--verify", "--end-of-options",
                             args.tag_name + "^{commit}").strip()
            tags = [commit]
            is_commit = True
        if not tags:
            print("No local tags match: %s\n"
                  "Fetch the genuine tag from the upstream repository that "
                  "publishes it, then rerun this command." % args.tag_name,
                  file=sys.stderr)
            return 1
        if not args.silent:
            print("number of tags to check: %d" % len(tags))

        best_tag, best_changes = None, None
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            for tag, changes in pool.map(compare_tag, tags):
                if not args.silent:
                    print("%s has %d lines changed" % (tag, changes))
                if best_changes is None or changes < best_changes:
                    best_tag, best_changes = tag, changes
                    if not args.silent:
                        print("%s is the new best match with %d lines changed"
                              % (tag, changes))
        print("Best match\n%s: %s\nLines changed: %d"
              % ("COMMIT" if is_commit else "TAG", best_tag, best_changes))
        return 0
    except (OSError, RuntimeError, ValueError) as error:
        print("Error: %s" % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
