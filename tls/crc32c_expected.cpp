// CRC-32C a "*_crc" client must obtain over the bytes the servers send. Without MANIFEST,
// the whole payload FILE (streaming, whole_transfer, tls). With MANIFEST, the message
// bodies of the framed servers: one per manifest size, cut from FILE in order; a size
// larger than FILE is cut to FILE's size, and a body that does not fit in what is left of
// FILE starts again at its beginning (the rule every framed server applies). Prints the
// CRC as 8 hex digits. Built by build.sh; run by bench_scenarios.py once per scenario,
// outside any measurement.
//
// Usage: crc32c_expected FILE [MANIFEST]

#include "consume_crc.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: %s FILE [MANIFEST]\n", argv[0]);
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    std::vector<char> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (file.empty()) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    const auto* payload = reinterpret_cast<const std::byte*>(file.data());

    consume::Crc32c crc;
    if (argc == 2) {
        crc.update(payload, file.size());
    } else {
        std::ifstream manifest(argv[2]);
        if (!manifest) {
            std::fprintf(stderr, "cannot open %s\n", argv[2]);
            return 1;
        }
        std::size_t offset = 0;
        for (std::string line; std::getline(manifest, line);) {
            const auto first = line.find_first_not_of(" \t");
            if (first == std::string::npos || line[first] == '#')
                continue;
            std::size_t size = std::stoull(line.substr(line.find_last_of(" \t") + 1));
            if (size > file.size())
                size = file.size();
            if (offset + size > file.size())
                offset = 0;
            crc.update(payload + offset, size);
            offset += size;
        }
    }
    std::printf("%08X\n", crc.value());
    return 0;
}
