// Replays every file of the directories (or files) given on the command
// line through LLVMFuzzerTestOneInput, without libFuzzer. Used by ctest to
// keep inputs that once crashed a parser as regression tests.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

static void Run(const std::filesystem::path& path, size_t& count) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
    ++count;
}

int main(int argc, char** argv) {
    size_t count = 0;
    for (int i = 1; i < argc; ++i) {
        std::filesystem::path root(argv[i]);
        if (std::filesystem::is_directory(root)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
                if (entry.is_regular_file()) Run(entry.path(), count);
        } else if (std::filesystem::exists(root)) {
            Run(root, count);
        }
    }
    std::printf("replayed %zu inputs\n", count);
    return 0;
}
