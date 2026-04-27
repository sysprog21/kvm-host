#!/usr/bin/env bash

set -e -u -o pipefail

cd "$(git rev-parse --show-toplevel)"

set -x

while IFS= read -r -d '' file; do
    clang-format-20 "$file" > expected-format
    diff -u -p --label="$file" --label="expected coding style" "$file" expected-format
done < <(git ls-files -z '*.c' '*.cxx' '*.cpp' '*.h' '*.hpp')

count=$(git ls-files -z '*.c' '*.cxx' '*.cpp' '*.h' '*.hpp' \
    | xargs -0 clang-format-20 --output-replacements-xml \
    | grep -c "</replacement>" || true)
exit "$count"
