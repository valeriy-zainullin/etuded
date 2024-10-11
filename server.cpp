#include <cassert>
#include <iostream>
#include <filesystem>

#include "LibLsp/JsonRpc/Endpoint.h"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc < 1 || argv[0] == nullptr) {
        std::cerr << "Invalid usage, missing executable path in argv.";
        return -1;
    }

    fs::path exec_path = fs::absolute(fs::path(argv[0]));
    fs::path exec_dir  = exec_path.parent_path();
    fs::path log_path  = exec_dir / "log.txt";

    // TODO: figure out compiler path!

    return 0;
}