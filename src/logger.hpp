#pragma once

#include "LibLsp/JsonRpc/MessageIssue.h"

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
