#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# Guard: every first-party source the build references must be IN the repository.
#
# WHY THIS EXISTS. `.gitignore` once carried the unanchored pattern `secrets/`,
# which git matches at any depth — so `src/secrets/` and
# `include/broker_exec/secrets/` were silently never committed. The working tree
# built fine; a clean clone died at CMake configure with
# "add_subdirectory given source \"src/secrets\" which is not an existing
# directory", and every CI job had been red since the first C++ commit.
#
# Two checks, because they catch the failure at two different moments:
#
#   (1) TRACKED — run from a full working tree (developer machine, pre-commit).
#       Any *.cpp/*.hpp/CMakeLists.txt under src/, include/ or tests/ that git
#       does not track is a file that will vanish on clone. Names the .gitignore
#       line responsible so the fix is obvious.
#
#   (2) RESOLVABLE — run from ANY checkout, including CI's tracked-files-only
#       one. Every add_subdirectory() path in every CMakeLists.txt must exist.
#       On CI this is the check that actually fires, and it fails with a
#       readable message instead of a CMake error 300 lines into a build.
#
# Exit 0 = clean. Exit 1 = a source is missing from the repository.
# ─────────────────────────────────────────────────────────────────────────────
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

status=0

# ── (1) Nothing first-party may be untracked ──
missing=()
while IFS= read -r f; do
  git ls-files --error-unmatch "$f" >/dev/null 2>&1 || missing+=("$f")
done < <(find src include tests -type f \
           \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name 'CMakeLists.txt' \) \
           2>/dev/null | sort)

if [ ${#missing[@]} -gt 0 ]; then
  echo "ERROR: first-party sources exist on disk but are NOT tracked by git."
  echo "       A clean clone will not contain them and will fail to configure."
  echo
  for f in "${missing[@]}"; do
    rule="$(git check-ignore -v "$f" 2>/dev/null || true)"
    if [ -n "$rule" ]; then
      echo "  $f"
      echo "      ignored by  ${rule%%	*}"
    else
      echo "  $f  (untracked — git add it)"
    fi
  done
  echo
  echo "Fix: anchor the offending .gitignore pattern to the repository root"
  echo "     (a leading '/'), then commit the files."
  status=1
fi

# ── (2) Every add_subdirectory() target must exist in this checkout ──
unresolved=()
while IFS= read -r listfile; do
  dir="$(dirname "$listfile")"
  while IFS= read -r sub; do
    # Skip generator expressions / variable references — not statically resolvable.
    case "$sub" in *'${'*|'') continue ;; esac
    [ -d "$dir/$sub" ] || unresolved+=("$listfile -> $sub")
  done < <(grep -oE '^[[:space:]]*add_subdirectory\([[:space:]]*[^ )]+' "$listfile" \
             | sed -E 's/^[[:space:]]*add_subdirectory\([[:space:]]*//' | tr -d '"')
done < <(git ls-files '*CMakeLists.txt')

if [ ${#unresolved[@]} -gt 0 ]; then
  echo "ERROR: add_subdirectory() references a directory that is not in this checkout:"
  for u in "${unresolved[@]}"; do echo "  $u"; done
  echo
  echo "The directory is probably matched by a .gitignore pattern and was never committed."
  status=1
fi

[ $status -eq 0 ] && echo "OK: all first-party sources are tracked and every add_subdirectory() resolves."
exit $status
