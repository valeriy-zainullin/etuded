#include <atomic>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <condition_variable>
#include <memory>
#include <variant>

// LibLsp.
#include "LibLsp/lsp/AbsolutePath.h"
#include "LibLsp/lsp/ProtocolJsonHandler.h"
#include "LibLsp/JsonRpc/stream.h"
#include "LibLsp/JsonRpc/RemoteEndPoint.h"
#include "LibLsp/JsonRpc/Endpoint.h"
#include "LibLsp/JsonRpc/Condition.h"
#include "LibLsp/lsp/general/initialize.h"
#include "LibLsp/lsp/general/initialized.h"
#include "LibLsp/lsp/general/exit.h"
#include "LibLsp/lsp/textDocument/did_open.h"
#include "LibLsp/lsp/textDocument/document_symbol.h"
#include "LibLsp/lsp/textDocument/declaration_definition.h"
#include "LibLsp/lsp/lsTextDocumentIdentifier.h" // Missing include inside of did_close.h, okay...
#include "LibLsp/lsp/lsDocumentUri.h"
#include "LibLsp/lsp/textDocument/did_close.h"

// Etude compiler.
#include "driver/compil_driver.hpp"
#include "driver/module.hpp"

#include "logger.hpp"
#include "lsp_visitor.hpp"

namespace fs = std::filesystem;
class ViewedFile {
public:
  ViewedFile(lsDocumentUri uri)
    : uri_(std::move(uri)), path_(uri_.GetAbsolutePath().path) {
      assert(path_.is_absolute());

      Invalidate();
  }

  void Invalidate() {
      // Компилятор на данный момент ищет файлы в рабочей директории.
      //   В том числе, все импортируемые. Кроме стандартной библиотеки,
      //   которую он найдет и так, если мы укажем переменную окружения.
      //   Потому сменим рабочую директорию. Другие части нашего кода от
      //   этого не зависят.

      // https://stackoverflow.com/a/57096619
      fs::current_path(path_.parent_path());

      std::string module_name = GetModuleName();
      // Важно, чтобы module_name существовал все время выполнения
      //   этой функции, потому что compilation driver
      //   принимает эту строку как std::string_view.
      CompilationDriver driver(module_name);
      driver.PrepareForTooling();

      symbols.clear();

      LSPVisitor visitor(std::string(path_), &symbols, &usages);
      driver.RunVisitor(&visitor);
  }
private:
  std::string GetModuleName() {
      // Module.et -> Module
      return path_.filename().replace_extension();
  }
public:
  lsDocumentUri uri_;
  fs::path path_;

  std::vector<lsDocumentSymbol> symbols;
  std::vector<SymbolUsage> usages;
};

std::unordered_map<std::string, ViewedFile> file_cache;

int main(int argc, char** argv) {
  if (argc < 1 || argv[0] == nullptr) {
    std::cerr << "Invalid usage, missing executable path in argv.";
    return -1;
  }

  fs::path exec_path = fs::absolute(fs::path(argv[0]));
  fs::path exec_dir  = exec_path.parent_path();
  fs::path stdlib_path  = exec_dir / "etude_stdlib";
  // Makes a copy of strings pointed by name and value.
  setenv("ETUDE_STDLIB", std::string(stdlib_path).c_str(), true);

  std::atomic<bool> initialized = false;
  std::atomic<bool> exiting = false;

  Logger logger;

  auto server_endpoint = std::make_shared<GenericEndpoint>(logger);
  // TODO: handlers.

  auto json_handler = std::make_shared<lsp::ProtocolJsonHandler>();
  RemoteEndPoint client_endpoint(json_handler, server_endpoint, logger, lsp::Standard, 1);

  // https://github.com/kuafuwang/LspCpp/blob/e0b443d42e7d23638d727ac8ef6839b9e527bf0a/examples/StdIOServerExample.cpp#L57
  client_endpoint.registerHandler([&](const td_initialize::request& request) {
    td_initialize::response response;
    
    response.id = request.id;
    response.result.capabilities = lsServerCapabilities {
        .definitionProvider = {{true, {}}},
        .documentSymbolProvider = {{true, {}}},
        .documentLinkProvider = lsDocumentLinkOptions {},
    };

    return response;
  });

  auto find_file = [&](const lsDocumentUri& uri) -> ViewedFile& {
    auto file_it = file_cache.find(uri.raw_uri_);
    if (file_it == file_cache.end()) {
      // Здесь произойдет разбор файла с путем doc_path.
      //   Внутри конструктора будет вызов Invalidate(), он
      //   разбирает файл и собарет информацию, которую
      //   отображает редактор.
      // TODO: в дальнейшем может понадобиться комплиировать
      //   буффер текста, а не файл с диска. Если он еще не был
      //   сохранен в редакторе.
      // NOTE: пока не понятно, как давать подсказки по дополнению.
      //   Нужно разобраться в алгоритме, как это работает в других
      //   случаях. Т.к. если код не дописан, будут ошибки со стороны
      //   парсера.
      auto file = ViewedFile(uri);

      auto result = file_cache.insert({uri.raw_uri_, std::move(file)});
      assert(result.second);
      file_it = std::move(result.first);
    }

    return file_it->second;
  };

  client_endpoint.registerHandler([&](const td_symbol::request& request) {
    auto& file_uri = request.params.textDocument.uri;
    ViewedFile& file = find_file(file_uri);

    td_symbol::response response;
    response.id = request.id;
    response.result = file.symbols;
    return response;
  });

  client_endpoint.registerHandler([&](const td_definition::request& request) {
    auto& file_uri = request.params.textDocument.uri;
    ViewedFile& file = find_file(file_uri);

    SymbolUsage* usage = nullptr;
    const lsPosition& editor_pos = request.params.position;
    for (auto& usage_item: file.usages) {
      // Токен не может продолжаться на следующей строке, перевод строки --
      //   разделитель. Потому можно смотреть на строку начала.
      if (usage_item.range.start.line != editor_pos.line) {
        continue;
      }

      // Разрешаем равенство, т.к. можно встать сразу после символа,
      //   это все еще разрешено. И после токена обычно пробельный символ,
      //   потому все ок.
      if (
        usage_item.range.start.character <= editor_pos.character &&
        editor_pos.character <= usage_item.range.end.character
      ) {
        assert(
          usage == nullptr &&
          "BUG: usages overlap (requested position is in both)."
        );
        usage = &usage_item;
      }
    }

    std::vector<LocationLink> locations; 
    if (usage != nullptr) {
      // Distinguish decl and def positions like done in cquery:
      //    https://github.com/jacobdufault/cquery/blob/9b80917cbf7d26b78ec62b409442ecf96f72daf9/src/messages/text_document_definition.cc#L96
      locations.push_back(LocationLink {
        targetUri: lsDocumentUri::FromPath(usage->declared_at.path),
        targetRange: lsRange(usage->declared_at.decl_position, usage->declared_at.decl_position),
        targetSelectionRange: lsRange(usage->declared_at.decl_position, usage->declared_at.decl_position),
      });
    }

    td_definition::response response;
    response.id = request.id;
    response.result = {{}, locations};

    return response;
  });

  client_endpoint.registerHandler([&](Notify_InitializedNotification::notify& notify) {
    initialized.store(true);
  });

  client_endpoint.registerHandler([&](Notify_Exit::notify& notify) {
    client_endpoint.stop();
    exiting.store(true);
  });

  client_endpoint.registerHandler([&](Notify_TextDocumentDidOpen::notify& notify) {
    if (!initialized) {
        return;
    }
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