#!/usr/bin/env bash

set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

# clang-format-20 -n --Werror exits non-zero on the first violation, which
# is exactly what we want from CI. Drops the temp-file + diff loop the old
# script used (which also wrapped exit codes >255).
sources=()
while IFS= read -r f; do
    sources+=("$f")
done < <(git ls-files '*.c' '*.cxx' '*.cpp' '*.h' '*.hpp' \
         | grep -v '^src/dtc/')

clang-format-20 -n --Werror "${sources[@]}"
