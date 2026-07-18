#!/bin/sh

set -eu

usage() {
    printf '%s\n' \
        'usage: package_macos.sh --build-dir <dir> --output-dir <dir>' >&2
    exit 2
}

build_directory=
output_directory=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --build-dir)
            [ "$#" -ge 2 ] || usage
            build_directory=$2
            shift 2
            ;;
        --output-dir)
            [ "$#" -ge 2 ] || usage
            output_directory=$2
            shift 2
            ;;
        *) usage ;;
    esac
done

[ -n "$build_directory" ] && [ -n "$output_directory" ] || usage

script_directory=$(CDPATH= cd "$(dirname "$0")" && pwd)
repository_root=$(CDPATH= cd "$script_directory/.." && pwd)
build_directory=$(CDPATH= cd "$build_directory" && pwd)
mkdir -p "$output_directory"
output_directory=$(CDPATH= cd "$output_directory" && pwd)

version=$(sed -n 's/^project(ccs_trans VERSION \([0-9][0-9.]*\) LANGUAGES.*$/\1/p' \
    "$repository_root/CMakeLists.txt")
grep -q '^set(CCS_TRANS_VERSION_SUFFIX "[^"]*")$' "$repository_root/CMakeLists.txt" || {
    printf 'unable to read version suffix from CMakeLists.txt\n' >&2
    exit 1
}
version_suffix=$(sed -n 's/^set(CCS_TRANS_VERSION_SUFFIX "\([^"]*\)")$/\1/p' \
    "$repository_root/CMakeLists.txt")
[ -n "$version" ] || {
    printf 'unable to read project version from CMakeLists.txt\n' >&2
    exit 1
}
[ -z "$version_suffix" ] || {
    printf 'formal packaging is disabled for development version suffix %s\n' "$version_suffix" >&2
    exit 1
}
distribution=ccs-trans-$version-macOS-arm64
application_source="$build_directory/ccs-trans.app"
cli_source="$build_directory/ccs-trans"
stage_parent=$(mktemp -d "${TMPDIR:-/tmp}/ccs-trans-package.XXXXXX")
trap 'rm -rf "$stage_parent"' EXIT HUP INT TERM
stage="$stage_parent/$distribution"
mkdir -p "$stage/docs/Archived" "$stage/licenses"

[ -d "$application_source" ] || { printf 'missing app bundle: %s\n' "$application_source" >&2; exit 1; }
[ -x "$cli_source" ] || { printf 'missing CLI: %s\n' "$cli_source" >&2; exit 1; }

ditto "$application_source" "$stage/ccs-trans.app"
cp "$cli_source" "$stage/ccs-trans"
cp "$repository_root/README.md" "$stage/README.md"
for document in Design.md DevelopmentPlan.md ProjectStructure.md; do
    cp "$repository_root/docs/$document" "$stage/docs/$document"
done
for document in MacOSValidationCheckResult.md Reconstruction.md Release-0.5.0.md Release-0.6.0.md \
    Planning-0.7.0.md Release-0.7.0.md; do
    cp "$repository_root/docs/Archived/$document" "$stage/docs/Archived/$document"
done
cp "$repository_root/third_party/nlohmann/LICENSE.MIT" "$stage/licenses/nlohmann-LICENSE.MIT"
cp "$repository_root/third_party/sqlite/NOTICE.md" "$stage/licenses/sqlite-NOTICE.md"

codesign --force --options runtime --timestamp=none --sign - "$stage/ccs-trans"
codesign --force --options runtime --timestamp=none \
    --entitlements "$repository_root/packaging/macos/ccs-trans.entitlements" \
    --sign - "$stage/ccs-trans.app"
codesign --verify --deep --strict --verbose=2 "$stage/ccs-trans.app"
codesign --verify --strict --verbose=2 "$stage/ccs-trans"

(
    cd "$stage"
    find . -type f ! -name SHA256SUMS -print | LC_ALL=C sort | while IFS= read -r file; do
        shasum -a 256 "$file"
    done
) > "$stage/SHA256SUMS"

zip_path="$output_directory/$distribution.zip"
rm -f "$zip_path"
ditto -c -k --sequesterRsrc --keepParent "$stage" "$zip_path"
shasum -a 256 "$zip_path"
