#!/bin/sh

set -u

failures=0

ok() {
    printf '[ok] %s\n' "$1"
}

fail() {
    printf '[missing] %s\n' "$1" >&2
    failures=$((failures + 1))
}

require_command() {
    if command -v "$1" >/dev/null 2>&1; then
        ok "$1 -> $(command -v "$1")"
    else
        fail "required command is not available: $1"
    fi
}

script_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
repository_root=$(CDPATH= cd "$script_dir/.." && pwd)
icon_path="$repository_root/assets/icons/ccs-trans-512.png"

for command_name in \
    cmake ninja xcrun xcodebuild sw_vers sips iconutil plutil codesign security otool file awk
do
    require_command "$command_name"
done

if [ "$(uname -s 2>/dev/null || true)" = "Darwin" ]; then
    ok "host operating system is Darwin"
else
    fail "stage 13 requires a macOS host"
fi

machine=$(uname -m 2>/dev/null || true)
if [ "$machine" = "arm64" ]; then
    ok "host architecture is arm64"
else
    fail "stage 13 requires an Apple Silicon arm64 host; found ${machine:-unknown}"
fi

if command -v sw_vers >/dev/null 2>&1; then
    macos_version=$(sw_vers -productVersion 2>/dev/null || true)
    macos_major=${macos_version%%.*}
    if [ "$macos_major" = "26" ]; then
        ok "macOS version $macos_version"
    else
        fail "macOS 26 is required; found ${macos_version:-unknown}"
    fi
fi

if command -v xcodebuild >/dev/null 2>&1; then
    xcode_version=$(xcodebuild -version 2>/dev/null | awk 'NR == 1 { print $2 }')
    xcode_major=${xcode_version%%.*}
    case "$xcode_major" in
        ''|*[!0-9]*) fail "unable to determine the Xcode version" ;;
        *)
            if [ "$xcode_major" -ge 26 ]; then
                ok "Xcode $xcode_version"
            else
                fail "Xcode 26 or newer is required; found $xcode_version"
            fi
            ;;
    esac
fi

sdk_path=
if command -v xcrun >/dev/null 2>&1; then
    sdk_version=$(xcrun --sdk macosx --show-sdk-version 2>/dev/null || true)
    sdk_major=${sdk_version%%.*}
    sdk_path=$(xcrun --sdk macosx --show-sdk-path 2>/dev/null || true)
    if [ "$sdk_major" = "26" ] && [ -n "$sdk_path" ]; then
        ok "macOS SDK $sdk_version -> $sdk_path"
    else
        fail "the selected Xcode must provide the macOS 26 SDK; found ${sdk_version:-unknown}"
    fi
fi

if command -v cmake >/dev/null 2>&1; then
    cmake_version=$(cmake --version 2>/dev/null | awk 'NR == 1 { print $3 }')
    cmake_major=${cmake_version%%.*}
    cmake_rest=${cmake_version#*.}
    cmake_minor=${cmake_rest%%.*}
    case "$cmake_major.$cmake_minor" in
        *[!0-9.]*) fail "unable to determine the CMake version" ;;
        *)
            if [ "$cmake_major" -gt 3 ] || { [ "$cmake_major" -eq 3 ] && [ "$cmake_minor" -ge 20 ]; }; then
                ok "CMake $cmake_version"
            else
                fail "CMake 3.20 or newer is required; found $cmake_version"
            fi
            ;;
    esac
fi

if [ -f "$icon_path" ] && command -v sips >/dev/null 2>&1; then
    icon_info=$(sips -g pixelWidth -g pixelHeight -g hasAlpha "$icon_path" 2>/dev/null || true)
    icon_width=$(printf '%s\n' "$icon_info" | awk '/pixelWidth:/ { print $2 }')
    icon_height=$(printf '%s\n' "$icon_info" | awk '/pixelHeight:/ { print $2 }')
    icon_alpha=$(printf '%s\n' "$icon_info" | awk '/hasAlpha:/ { print $2 }')
    if [ "$icon_width" = "512" ] && [ "$icon_height" = "512" ] && [ "$icon_alpha" = "yes" ]; then
        ok "canonical icon is 512x512 with alpha"
    else
        fail "canonical icon must be 512x512 with alpha; found ${icon_width:-?}x${icon_height:-?}, alpha=${icon_alpha:-?}"
    fi
else
    fail "canonical icon is missing or cannot be inspected: $icon_path"
fi

temporary_directory=
if [ -n "$sdk_path" ] && command -v xcrun >/dev/null 2>&1; then
    temporary_directory=$(mktemp -d "${TMPDIR:-/tmp}/ccs-trans-stage13.XXXXXX") || exit 1
    trap 'rm -rf "$temporary_directory"' EXIT HUP INT TERM
    probe_source="$temporary_directory/probe.cpp"
    probe_binary="$temporary_directory/probe"
    printf '%s\n' \
        '#include <curl/curl.h>' \
        '#include <version>' \
        'static_assert(__cplusplus >= 202002L);' \
        'int main() { return curl_version_info(CURLVERSION_NOW) == nullptr; }' \
        > "$probe_source"
    if xcrun --sdk macosx clang++ \
        -std=c++20 -arch arm64 -mmacosx-version-min=26.0 -isysroot "$sdk_path" \
        "$probe_source" -lcurl -o "$probe_binary"
    then
        ok "Apple Clang C++20 and SDK system libcurl compile/link probe"
        binary_info=$(file "$probe_binary" 2>/dev/null || true)
        case "$binary_info" in
            *arm64*) ok "probe binary architecture is arm64" ;;
            *) fail "probe binary is not arm64: $binary_info" ;;
        esac
        curl_dependency=$(otool -L "$probe_binary" 2>/dev/null | awk '/\/usr\/lib\/libcurl/ { print $1; exit }')
        if [ -n "$curl_dependency" ]; then
            ok "probe links system libcurl -> $curl_dependency"
        else
            fail "probe does not link /usr/lib/libcurl"
        fi
    else
        fail "Apple Clang could not compile and link the C++20/system-libcurl probe"
    fi
fi

if command -v xcrun >/dev/null 2>&1; then
    for tool_name in notarytool stapler; do
        if tool_path=$(xcrun --find "$tool_name" 2>/dev/null); then
            ok "$tool_name -> $tool_path"
        else
            fail "Xcode tool is unavailable: $tool_name"
        fi
    done
fi

if command -v security >/dev/null 2>&1; then
    if security find-identity -v -p codesigning 2>/dev/null | grep 'Developer ID Application' >/dev/null 2>&1; then
        ok "Developer ID Application identity is available"
    else
        printf '[release-pending] Developer ID Application identity is not available in this keychain\n'
    fi
fi

if [ "$failures" -ne 0 ]; then
    printf 'Stage 13 prerequisites failed with %s issue(s).\n' "$failures" >&2
    exit 1
fi

printf 'Stage 13 prerequisites are available.\n'
