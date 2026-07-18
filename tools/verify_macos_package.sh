#!/bin/sh

set -eu

script_directory=$(CDPATH= cd "$(dirname "$0")" && pwd)
repository_root=$(CDPATH= cd "$script_directory/.." && pwd)
version=$(sed -n 's/^project(ccs_trans VERSION \([0-9][0-9.]*\) LANGUAGES.*$/\1/p' \
    "$repository_root/CMakeLists.txt")
[ -n "$version" ] || {
    printf 'unable to read project version from CMakeLists.txt\n' >&2
    exit 1
}

if [ "$#" -ne 1 ]; then
    printf 'usage: %s <ccs-trans-%s-macOS-arm64.zip>\n' "$0" "$version" >&2
    exit 2
fi

archive=$1
expected_name=ccs-trans-$version-macOS-arm64.zip
archive_name=$(basename "$archive")
[ "$archive_name" = "$expected_name" ] || {
    printf 'unexpected archive name: %s\n' "$(basename "$archive")" >&2
    exit 1
}

temporary_directory=$(mktemp -d "${TMPDIR:-/tmp}/ccs-trans-verify.XXXXXX")
trap 'rm -rf "$temporary_directory"' EXIT HUP INT TERM
ditto -x -k "$archive" "$temporary_directory"
root="$temporary_directory/ccs-trans-$version-macOS-arm64"
[ -d "$root/ccs-trans.app" ] && [ -x "$root/ccs-trans" ] || {
    printf 'archive is missing the app or CLI\n' >&2
    exit 1
}

actual=$(find "$root" -mindepth 1 -maxdepth 1 -print | sed "s|$root/||" | LC_ALL=C sort)
expected=$(printf '%s\n' README.md SHA256SUMS ccs-trans ccs-trans.app docs licenses | LC_ALL=C sort)
[ "$actual" = "$expected" ] || {
    printf 'archive top-level whitelist mismatch\n' >&2
    printf '%s\n' "$actual" >&2
    exit 1
}

actual_licenses=$(find "$root/licenses" -mindepth 1 -maxdepth 1 -type f -exec basename {} \; | LC_ALL=C sort)
expected_licenses=$(printf '%s\n' nlohmann-LICENSE.MIT sqlite-NOTICE.md | LC_ALL=C sort)
[ "$actual_licenses" = "$expected_licenses" ] || {
    printf 'archive license whitelist mismatch\n' >&2
    printf '%s\n' "$actual_licenses" >&2
    exit 1
}

actual_documents=$(find "$root/docs" -type f | sed "s|$root/docs/||" | LC_ALL=C sort)
expected_documents=$(printf '%s\n' \
    Archived/MacOSValidationCheckResult.md \
    Archived/Planning-0.7.0.md \
    Archived/Reconstruction.md \
    Archived/Release-0.5.0.md \
    Archived/Release-0.6.0.md \
    Archived/Release-0.7.0.md \
    Design.md DevelopmentPlan.md ProjectStructure.md | LC_ALL=C sort)
[ "$actual_documents" = "$expected_documents" ] || {
    printf 'archive document whitelist mismatch\n' >&2
    printf '%s\n' "$actual_documents" >&2
    exit 1
}

(
    cd "$root"
    shasum -a 256 -c SHA256SUMS
)
codesign --verify --deep --strict --verbose=2 "$root/ccs-trans.app"
codesign --verify --strict --verbose=2 "$root/ccs-trans"

for signed_path in "$root/ccs-trans.app" "$root/ccs-trans"; do
    signature_details=$(codesign -dv --verbose=4 "$signed_path" 2>&1)
    if ! printf '%s\n' "$signature_details" | grep -Fqx 'Signature=adhoc'; then
        printf 'package entry is not ad-hoc signed: %s\n' "$signed_path" >&2
        exit 1
    fi
done

for binary in "$root/ccs-trans" "$root/ccs-trans.app/Contents/MacOS/ccs-trans"; do
    [ "$(lipo -archs "$binary")" = arm64 ] || {
        printf 'binary is not arm64-only: %s\n' "$binary" >&2
        exit 1
    }
done

if find "$root" -name '*.dSYM' -o -name '*.iconset' -o -name '.ccs-trans' \
    -o -name '*.log' | grep . >/dev/null; then
    printf 'archive contains a forbidden generated or user-data path\n' >&2
    exit 1
fi

printf 'macOS package verification ok\n'
