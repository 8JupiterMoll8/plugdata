#!/usr/bin/env bash
# Apply the fork's submodule patches on top of the pinned upstream submodules.
#
# The fork's submodule commits (pure-data, pd-else, pd-lua, JUCE) do not exist
# on any public remote, so the superproject pins the UPSTREAM commits and keeps
# our changes as patch files in patches/submodules/<submodule>/. A fresh clone:
#
#   git clone <fork> && cd <fork>
#   git submodule update --init
#   scripts/apply-submodule-patches.sh
#
# Safe to re-run: a patch that is already applied is skipped. A patch that
# neither applies nor is already applied stops the script (nothing is guessed).
#
# The patched submodules then differ from their pins, so the superproject shows
# them as modified. Hide that locally with:
#   git config submodule.<name>.ignore all
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
patch_root="$root/patches/submodules"
applied=0
skipped=0

if [ ! -d "$patch_root" ]; then
  echo "error: $patch_root not found" >&2
  exit 1
fi

for dir in "$patch_root"/*/; do
  name="$(basename "$dir")"
  sub="$root/Libraries/$name"
  if [ ! -e "$sub/.git" ]; then
    echo "error: $sub is not checked out; run 'git submodule update --init' first" >&2
    exit 1
  fi
  for patch in "$dir"*.patch; do
    label="$name/$(basename "$patch")"
    if git -C "$sub" apply --check --reverse "$patch" >/dev/null 2>&1; then
      echo "skip    $label (already applied)"
      skipped=$((skipped + 1))
    elif git -C "$sub" apply --check "$patch" >/dev/null 2>&1; then
      # --keep-cr: some submodule files (JUCE) are CRLF; `am` would otherwise
      # strip the CR from the patch's context lines and fail to match.
      if ! git -C "$sub" -c user.name="plugdata-fork" -c user.email="fork@localhost" am --quiet --keep-cr "$patch"; then
        git -C "$sub" am --abort >/dev/null 2>&1 || true
        echo "error: git am failed for $label (aborted, $name left unchanged)" >&2
        exit 1
      fi
      echo "applied $label"
      applied=$((applied + 1))
    else
      echo "error: $label neither applies nor is already applied (is $name at its pinned commit?)" >&2
      exit 1
    fi
  done
done

echo "done: $applied applied, $skipped already applied"
