#include "core/sha256.hpp"

#include <cstdlib>
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
    std::cout << "SHA-256 tests passed\n";
    return 0;
}
