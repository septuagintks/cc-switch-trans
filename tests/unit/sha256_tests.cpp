#include "core/sha256.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main() {
    require(
        ccs::sha256_hex("")
            == "e3b0c44298fc1c149afbf4c8996fb924"
               "27ae41e4649b934ca495991b7852b855",
        "empty SHA-256 vector");
    require(
        ccs::sha256_hex("abc")
            == "ba7816bf8f01cfea414140de5dae2223"
               "b00361a396177a9cb410ff61f20015ad",
        "abc SHA-256 vector");
    require(
        ccs::sha256_hex("abcdbcdecdefdefgefghfghighijhijk"
                        "ijkljklmklmnlmnomnopnopq")
            == "248d6a61d20638b8e5c026930c3e6039"
               "a33ce45964ff2167f6ecedd419db06c1",
        "multi-block SHA-256 vector");
    require(
        ccs::sha256_hex(std::string(1'000'000, 'a'))
            == "cdc76e5c9914fb9281a1c7e284d73e67"
               "f1809a48a497200e046d39ccc7112cd0",
        "million-a SHA-256 vector");
    const auto file = std::filesystem::temp_directory_path()
        / "ccs-trans-sha256-streaming-test.bin";
    const std::string file_content = std::string(70'000, 'a') + "stream tail";
    {
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output), "open streaming SHA-256 fixture");
        output.write(file_content.data(), static_cast<std::streamsize>(file_content.size()));
    }
    std::string file_digest;
    std::string error;
    require(ccs::sha256_file_hex(file, file_digest, error), error);
    require(file_digest == ccs::sha256_hex(file_content),
        "streaming file SHA-256 matches in-memory implementation across chunks");
    std::error_code ec;
    std::filesystem::remove(file, ec);
    std::cout << "SHA-256 tests passed\n";
    return 0;
}
