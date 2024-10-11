#include <atomic>
#include <cassert>
#include <iostream>
#include <filesystem>
#include <condition_variable>
#include <memory>

#include "LibLsp/lsp/ProtocolJsonHandler.h"
#include "LibLsp/JsonRpc/stream.h"
#include "LibLsp/JsonRpc/RemoteEndPoint.h"
#include "LibLsp/JsonRpc/Endpoint.h"
#include "LibLsp/JsonRpc/Condition.h"

class Logger final : public lsp::Log {
    void log(Level level, std::wstring&& msg) {
        return log(level, msg);
    }

    void log(Level level, const std::wstring& msg) {
        std::ignore = level;
        std::wcerr << msg << '\n';
        std::wcerr.flush();
    }

    void log(Level level, std::string&& msg) {
        return log(level, msg); // log(Level, const std::string& msg);
    }

    void log(Level level, const std::string& msg) {
        std::ignore = level;
        std::cerr << msg << '\n';
        std::cerr.flush();
    }
};

int main(int argc, char** argv) {
    if (argc < 1 || argv[0] == nullptr) {
        std::cerr << "Invalid usage, missing executable path in argv.";
        return -1;
    }

    namespace fs = std::filesystem;
    fs::path exec_path = fs::absolute(fs::path(argv[0]));
    fs::path exec_dir  = exec_path.parent_path();
    fs::path compiler_path  = exec_dir / "etude" / "123";
    // TODO: figure out compiler path!

    std::atomic<bool> exiting = false;

    Logger logger;

    auto server_endpoint = std::make_shared<GenericEndpoint>(logger);
    // TODO: handlers.

    auto json_handler = std::make_shared<lsp::ProtocolJsonHandler>();
    RemoteEndPoint client_endpoint(json_handler, server_endpoint, logger);

    auto input  = std::make_shared<lsp::base_istream<decltype(std::cin)>>(std::cin);
    auto output = std::make_shared<lsp::base_ostream<decltype(std::cout)>>(std::cout);
    client_endpoint.startProcessingMessages(input, output);

    // cppreference: "These functions are guaranteed to return only if
    //   value has changed, even if underlying implementation unblocks
    //   spuriously."
    // https://en.cppreference.com/w/cpp/atomic/atomic/wait
    exiting.wait(false);

    return 0;
}