#!/usr/bin/env bash

set -Eeuo pipefail

# Compiles and runs one C++ unit-test file from the project's test directory.
# The test filename is mandatory. The test folder is optional.

show_usage() {
    printf 'Usage: %s <test-file.cpp> [test-folder]\n' "$(basename "$0")"
    printf '\n'
    printf 'Examples:\n'
    printf '  %s test_best_first_search.cpp BestFirstSearch\n' "$(basename "$0")"
    printf '  %s test_best_first_search.cpp\n' "$(basename "$0")"
    printf '  %s test/BestFirstSearch/test_best_first_search.cpp\n' "$(basename "$0")"
    printf '\n'
    printf 'The executable is created beside the selected test source file.\n'
}

fail() {
    printf 'ERROR: %s\n' "$1" >&2
    exit 1
}

find_project_root() {
    local current_directory
    current_directory="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"

    while [[ "$current_directory" != "/" ]]; do
        if [[ -d "$current_directory/lib" && -d "$current_directory/test" ]]; then
            printf '%s\n' "$current_directory"
            return 0
        fi
        current_directory="$(dirname -- "$current_directory")"
    done

    return 1
}

resolve_given_folder() {
    local project_root="$1"
    local folder_argument="$2"
    local candidate

    if [[ "$folder_argument" = /* ]]; then
        candidate="$folder_argument"
    elif [[ -d "$project_root/test/$folder_argument" ]]; then
        candidate="$project_root/test/$folder_argument"
    elif [[ -d "$project_root/$folder_argument" ]]; then
        candidate="$project_root/$folder_argument"
    else
        return 1
    fi

    cd -- "$candidate" && pwd -P
}

resolve_test_source() {
    local project_root="$1"
    local file_argument="$2"
    local folder_argument="${3:-}"
    local candidate
    local test_directory
    local -a matches=()

    if [[ -n "$folder_argument" ]]; then
        test_directory="$(resolve_given_folder "$project_root" "$folder_argument")" ||
            fail "Test folder '$folder_argument' does not exist."
        candidate="$test_directory/$(basename -- "$file_argument")"
        [[ -f "$candidate" ]] ||
            fail "Test file '$(basename -- "$file_argument")' was not found in '$test_directory'."
        printf '%s\n' "$candidate"
        return 0
    fi

    if [[ "$file_argument" = /* && -f "$file_argument" ]]; then
        printf '%s\n' "$file_argument"
        return 0
    fi

    if [[ -f "$project_root/$file_argument" ]]; then
        printf '%s\n' "$project_root/$file_argument"
        return 0
    fi

    if [[ -f "$project_root/test/$file_argument" ]]; then
        printf '%s\n' "$project_root/test/$file_argument"
        return 0
    fi

    while IFS= read -r -d '' candidate; do
        matches+=("$candidate")
    done < <(find "$project_root/test" -type f -name "$(basename -- "$file_argument")" -print0 | sort -z)

    if (( ${#matches[@]} == 0 )); then
        fail "Test file '$file_argument' was not found under '$project_root/test'."
    fi

    if (( ${#matches[@]} > 1 )); then
        printf 'ERROR: More than one test named %s was found:\n' "$file_argument" >&2
        printf '  %s\n' "${matches[@]}" >&2
        fail "Pass the test folder as the second argument."
    fi

    printf '%s\n' "${matches[0]}"
}

if (( $# == 0 )); then
    show_usage
    exit 2
fi

if [[ "$1" == "-h" || "$1" == "--help" ]]; then
    show_usage
    exit 0
fi

if (( $# > 2 )); then
    show_usage >&2
    fail "Expected one mandatory argument and no more than one optional argument."
fi

readonly TEST_FILE_ARGUMENT="$1"
readonly TEST_FOLDER_ARGUMENT="${2:-}"

case "$TEST_FILE_ARGUMENT" in
    *.cpp|*.cc|*.cxx) ;;
    *) fail "The mandatory test file must end in .cpp, .cc, or .cxx." ;;
esac

PROJECT_ROOT="$(find_project_root)" ||
    fail "Could not find a project root containing both lib/ and test/."
readonly PROJECT_ROOT

TEST_SOURCE="$(resolve_test_source "$PROJECT_ROOT" "$TEST_FILE_ARGUMENT" "$TEST_FOLDER_ARGUMENT")"
readonly TEST_SOURCE

TEST_DIRECTORY="$(cd -- "$(dirname -- "$TEST_SOURCE")" && pwd -P)"
readonly TEST_DIRECTORY
readonly TEST_ROOT="$(cd -- "$PROJECT_ROOT/test" && pwd -P)"

case "$TEST_DIRECTORY/" in
    "$TEST_ROOT/"*) ;;
    *) fail "The selected test file must be inside '$TEST_ROOT'." ;;
esac

MODULE_PATH="${TEST_DIRECTORY#"$TEST_ROOT"/}"
if [[ "$TEST_DIRECTORY" == "$TEST_ROOT" ]]; then
    fail "Place the test in a module folder such as test/BestFirstSearch/."
fi
readonly MODULE_PATH

readonly LIBRARY_DIRECTORY="$PROJECT_ROOT/lib/$MODULE_PATH"
[[ -d "$LIBRARY_DIRECTORY" ]] ||
    fail "Matching library folder '$LIBRARY_DIRECTORY' does not exist."

readonly TEST_BASENAME="$(basename -- "$TEST_SOURCE")"
readonly EXECUTABLE_NAME="${TEST_BASENAME%.*}"
readonly EXECUTABLE_PATH="$TEST_DIRECTORY/$EXECUTABLE_NAME"

readonly CXX_COMPILER="${CXX:-g++}"
command -v "$CXX_COMPILER" >/dev/null 2>&1 ||
    fail "C++ compiler '$CXX_COMPILER' was not found."

# lib/ groups classes into shared domain folders (e.g. NodeManager holds
# many single-responsibility classes, some further nested under
# NodeManager/Central or NodeManager/Smart by which firmware role owns
# them) rather than one class per folder, so this cannot simply compile
# every .cpp sitting in a matched folder - that would sweep in unrelated
# ESP32-only siblings (e.g. an EspNowCommunication.cpp needing esp_now.h)
# that the test never actually needed. Instead it resolves dependencies
# per header file: starting from the test source, every local
# #include "X.h" is located anywhere under lib/ (however deeply nested),
# its sibling X.cpp (if any) is compiled, and both are scanned in turn -
# transitively pulling in exactly the classes actually referenced, same
# as PlatformIO's own chain-mode Library Dependency Finder does for the
# real ESP32 build.
declare -A RESOLVED_SOURCE_FILES=()
declare -A RESOLVED_HEADER_NAMES=()
declare -A ADDED_INCLUDE_DIRECTORIES=()
declare -a LIBRARY_SOURCES=()
declare -a INCLUDE_SCAN_QUEUE=()
declare -a INCLUDE_FLAGS=()

add_include_directory() {
    local directory="$1"
    if [[ -z "${ADDED_INCLUDE_DIRECTORIES[$directory]:-}" ]]; then
        ADDED_INCLUDE_DIRECTORIES["$directory"]=1
        INCLUDE_FLAGS+=("-I$directory")
    fi
}

resolve_included_header() {
    local header_name="$1"
    local header_path header_dir base ext candidate

    [[ -n "${RESOLVED_HEADER_NAMES[$header_name]:-}" ]] && return 0
    RESOLVED_HEADER_NAMES["$header_name"]=1

    header_path="$(find "$PROJECT_ROOT/lib" -type f -name "$header_name" -print -quit)"
    [[ -n "$header_path" ]] || return 0

    header_dir="$(dirname -- "$header_path")"
    add_include_directory "$header_dir"

    base="${header_name%.h}"
    for ext in cpp cc cxx; do
        candidate="$header_dir/$base.$ext"
        if [[ -f "$candidate" && -z "${RESOLVED_SOURCE_FILES[$candidate]:-}" ]]; then
            RESOLVED_SOURCE_FILES["$candidate"]=1
            LIBRARY_SOURCES+=("$candidate")
            INCLUDE_SCAN_QUEUE+=("$candidate")
        fi
    done

    INCLUDE_SCAN_QUEUE+=("$header_path")
}

INCLUDE_SCAN_QUEUE+=("$TEST_SOURCE")

while (( ${#INCLUDE_SCAN_QUEUE[@]} > 0 )); do
    scan_file="${INCLUDE_SCAN_QUEUE[0]}"
    INCLUDE_SCAN_QUEUE=("${INCLUDE_SCAN_QUEUE[@]:1}")

    [[ -f "$scan_file" ]] || continue

    while IFS= read -r included_header; do
        resolve_included_header "$included_header"
    done < <(grep -ohE '#include[[:space:]]*"[A-Za-z0-9_]+\.h"' "$scan_file" |
              sed -E 's/.*"([A-Za-z0-9_]+\.h)"/\1/')
done

(( ${#LIBRARY_SOURCES[@]} > 0 )) ||
    fail "No C++ implementation file under 'lib/' matched any header included from '$TEST_SOURCE'."

add_include_directory "$LIBRARY_DIRECTORY"
add_include_directory "$TEST_DIRECTORY"
[[ -d "$PROJECT_ROOT/include" ]] && add_include_directory "$PROJECT_ROOT/include"
[[ -d "$LIBRARY_DIRECTORY/include" ]] && add_include_directory "$LIBRARY_DIRECTORY/include"

declare -a COMPILER_FLAGS=(
    -std=c++17
    -O2
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -fno-exceptions
    -fno-rtti
)

printf 'Project root: %s\n' "$PROJECT_ROOT"
printf 'Test source:  %s\n' "$TEST_SOURCE"
printf 'Library:      %s\n' "$LIBRARY_DIRECTORY"
printf 'Executable:   %s\n' "$EXECUTABLE_PATH"
printf '\nCompiling...\n'

"$CXX_COMPILER" \
    "${COMPILER_FLAGS[@]}" \
    "${INCLUDE_FLAGS[@]}" \
    "${LIBRARY_SOURCES[@]}" \
    "$TEST_SOURCE" \
    -o "$EXECUTABLE_PATH"

printf 'Compilation passed.\n'
printf '\nRunning %s...\n\n' "$EXECUTABLE_NAME"

(
    cd -- "$TEST_DIRECTORY"
    "./$EXECUTABLE_NAME"
)

printf '\nTest execution passed.\n'
printf 'Executable retained at: %s\n' "$EXECUTABLE_PATH"