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

#include "LibLsp/lsp/general/exit.h"
#include "LibLsp/lsp/textDocument/did_open.h"
#include "LibLsp/lsp/textDocument/did_close.h"

class Logger final : public lsp::Log {
public:
    void log(Level level, std::wstring&& msg) override {
        return log(level, msg);
    }

    void log(Level level, const std::wstring& msg) override {
        std::ignore = level;
        std::wcerr << msg << '\n';
        std::wcerr.flush();
    }

    void log(Level level, std::string&& msg) override {
        return log(level, msg); // log(Level, const std::string& msg);
    }

    void log(Level level, const std::string& msg) override {
        std::ignore = level;
        std::cerr << msg << '\n';
        std::cerr.flush();
    }
};

template <typename T>
class istream : public lsp::base_istream<T> {
    using lsp::base_istream<T>::base_istream;

public:
    std::string what() override {
        return std::string();
    }

    virtual ~istream() = default;
};

template <typename T>
class ostream : public lsp::base_ostream<T> {
    using lsp::base_ostream<T>::base_ostream;

public:
    std::string what() override {
        return std::string();
    }

    virtual ~ostream() = default;
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

    client_endpoint.registerHandler([&](Notify_Exit::notify& notify) {
        client_endpoint.stop();
        exiting.store(true);
    });

    client_endpoint.registerHandler([&](Notify_TextDocumentDidOpen::notify& notify) {
        // Т.е. перед нами может быть даже не локальный файл, а на удаленном компьютере!
        logger.log(lsp::Log::Level::INFO, "opened file with uri " + notify.params.textDocument.uri.raw_uri_);
    });

    auto input  = std::static_pointer_cast<lsp::istream>(std::make_shared<istream<decltype(std::cin)>>(std::cin));
    auto output = std::static_pointer_cast<lsp::ostream>(std::make_shared<ostream<decltype(std::cout)>>(std::cout));
    client_endpoint.startProcessingMessages(input, output);

    // cppreference: "These functions are guaranteed to return only if
    //   value has changed, even if underlying implementation unblocks
    //   spuriously."
    // https://en.cppreference.com/w/cpp/atomic/atomic/wait
    exiting.wait(false);

    return 0;
}