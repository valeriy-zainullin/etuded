#include <atomic>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <condition_variable>
#include <memory>
#include <variant>
#include <sstream>

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
#include "LibLsp/lsp/textDocument/publishDiagnostics.h"
#include "LibLsp/lsp/textDocument/did_change.h"
#include "LibLsp/lsp/textDocument/did_save.h"
#include "LibLsp/lsp/textDocument/highlight.h"
#include "LibLsp/lsp/utils.h"

// Etude compiler.
#include "driver/compil_driver.hpp"
#include "driver/module.hpp"

#include "logger.hpp"
#include "lsp_visitor.hpp"

namespace fs = std::filesystem;

class LSPCompilationDriver final : public CompilationDriver {
  using CompilationDriver::CompilationDriver;

  virtual std::stringstream OpenFile(std::string_view name) override;

public:
  void PrepareForTooling() {
    ParseAllModules();
    RegisterSymbols();

    // Those in the beginning have the least dependencies (see TopSort(...))
    for (size_t i = 0; i < modules_.size(); i += 1) {
      ProcessModule(&modules_[i]);
    }

    for (auto& m : modules_) {
      m.InferTypes(solver_);
    }

    if (test_build) {
      FMT_ASSERT(modules_.back().GetName() == main_module_,
                  "Last module should be the main one");
      return;
    }
  }
  
  void RunVisitor(Visitor* visitor) {
    // Модуль, который был основным, находится в конце списка модулей
    //   после тополнической сортировки. Т.к. в него все ребра входили,
    //   но никакие не выходили: если кто-то его импортирует, мы об этом
    //   не знаем.

    modules_.back().RunTooling(visitor);
  }
};

class ViewedFile {
public:
  ViewedFile(lsDocumentUri uri)
    : uri_(std::move(uri)), abs_path_(uri_.GetAbsolutePath().path) {
      assert(abs_path_.is_absolute());

      Invalidate();
  }

  void Invalidate() {
      // Компилятор на данный момент ищет файлы в рабочей директории.
      //   В том числе, все импортируемые. Кроме стандартной библиотеки,
      //   которую он найдет и так, если мы укажем переменную окружения.
      //   Потому сменим рабочую директорию. Другие части нашего кода от
      //   этого не зависят.

      // Это и упрощение логики являются причинами однопоточного подхода.
      //   Его производительности хватает, а сложности, которые он
      //   создаст, в алгоритма и внутри компилятора (там есть 
      //   глобальные переменные) перевешивают необходимость.

      // https://stackoverflow.com/a/57096619
      fs::current_path(abs_path_.parent_path());

      std::string module_name = GetModuleName();
      diagnostic.reset();

      try {
        // Важно, чтобы module_name существовал все время выполнения
        //   этой функции, потому что compilation driver
        //   принимает эту строку как std::string_view.
        LSPCompilationDriver driver(module_name);

        driver.PrepareForTooling();

        LSPVisitor visitor(std::string(abs_path_), &symbols, &usages);

        symbols.clear();
        usages.clear();
        diagnostic.reset();

        driver.RunVisitor(&visitor);
      } catch (const ErrorAtLocation& err) {
        diagnostic = lsDiagnostic{
          range: lsRange{
            LsPositionFromLexLocation(err.where()),
            LsPositionFromLexLocation(err.where())
          },
          severity: lsDiagnosticSeverity::Error,
          message: std::string(err.what()),
        };
        return;
      } catch (const std::exception& exc) {
        diagnostic = lsDiagnostic{
          range: lsRange{lsPosition{0, 0}, lsPosition{0, 0}},
          severity: lsDiagnosticSeverity::Error,
          message: std::string(exc.what()),
        };
        return;
      }
  }

  void InvalidateOnLookup() {
    invalidate_on_lookup = true;
  }

  void Lookup() {
    if (invalidate_on_lookup) {
      Invalidate();
      invalidate_on_lookup = false;
    }
  }
private:
  std::string GetModuleName() {
      // Module.et -> Module
      return abs_path_.filename().replace_extension();
  }
public:
  lsDocumentUri uri_;
  fs::path abs_path_;

  std::optional<lsDiagnostic> diagnostic;
  std::vector<lsDocumentSymbol> symbols;
  std::vector<SymbolUsage> usages;

  std::optional<std::string> unsaved_content;

  bool invalidate_on_lookup = false;
};

std::unordered_map<std::string, ViewedFile> file_cache;

std::stringstream LSPCompilationDriver::OpenFile(std::string_view name) {
  auto rel_path = std::string(name) + ".et";

  // Also forcing lowercase on windows.
  // TODO: check vscode extension works on windows.
  std::string abs_path = lsp::NormalizePath(rel_path, false);
  auto it = file_cache.find(abs_path);
  if (it != file_cache.end()) {
    auto& file = it->second;

    if (file.unsaved_content.has_value()) {
      return std::stringstream(file.unsaved_content.value());
    }
  } 
  
  return CompilationDriver::OpenFile(name);
}

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
        .textDocumentSync = {{{}, lsTextDocumentSyncOptions{
          .openClose = true,
          .change = lsTextDocumentSyncKind::Full,
          .save = lsSaveOptions {
            .includeText = false
          },
        }}},
        .definitionProvider = {{true, {}}},
        .documentHighlightProvider = {{true, {}}},
        .documentSymbolProvider = {{true, {}}},
        .documentLinkProvider = lsDocumentLinkOptions {},
    };

    return response;
  });

  auto update_diagnostics = [&](const ViewedFile& file) {
    Notify_TextDocumentPublishDiagnostics::notify notify;
    notify.params.uri = file.uri_;
    if (file.diagnostic.has_value()) {
      notify.params.diagnostics.push_back(file.diagnostic.value());
    }

    client_endpoint.sendNotification(notify);
  };

  auto find_file = [&](const lsDocumentUri& uri) -> ViewedFile& {
    auto file_it = file_cache.find(uri.GetAbsolutePath().path);
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

      // Нам нужен путь, т.к. при открытии файла мы смотрим
      //   в этот кеш, удобнее оперировать путями, чем uri.
      //   Возможно, именно мне сейчас..
      // На windows все приводится к нижнему регистру,
      //   внутри GetAbsolutePath() есть вызов
      //   lsp::NormalizePath, там есть параметр
      //   force_lower_on_windows. чтобы
      //   не хранить один файл дважды. Ведь еще есть
      //   чтение кеша при импортировании модулей внутри
      //   etude. А там модули могут быть и большими,
      //   и маленькими быть написаны. И на винде будет
      //   компилироваться.. Тогда на винде просто везде
      //   сделаем маленькими.
      // На самом деле, это свойство файловой системы,
      //   она может быть чувствительна или нет к регистру.
      // Но все эти ухищрения только для случая, где
      //   у кого-то неправильный регистр.

      auto result = file_cache.insert({uri.GetAbsolutePath().path, std::move(file)});
      assert(result.second);
      file_it = std::move(result.first);
    }

    ViewedFile& file = file_it->second;

    file.Lookup();
    update_diagnostics(file); // Cheap, can do on each request or notification.

    return file;
  };

  auto close_file = [&](const lsDocumentUri& uri) {
    file_cache.erase(uri.GetAbsolutePath().path);
  };

  client_endpoint.registerHandler([&](const td_symbol::request& request) {
    auto& file_uri = request.params.textDocument.uri;
    ViewedFile& file = find_file(file_uri);

    td_symbol::response response;
    response.id = request.id;
    if (!file.diagnostic.has_value()) {
      response.result = file.symbols;
    }
    return response;
  });

  client_endpoint.registerHandler([&](const td_definition::request& request) {
    auto& file_uri = request.params.textDocument.uri;
    ViewedFile& file = find_file(file_uri);

    td_definition::response response;
    response.id = request.id;

    if (file.diagnostic.has_value()) {
      return response;
    }

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

    response.result = {{}, locations};

    return response;
  });

  client_endpoint.registerHandler([&](const td_highlight::request& request) {
    auto& file_uri = request.params.textDocument.uri;
    ViewedFile& file = find_file(file_uri);

    td_highlight::response response;
    response.id = request.id;

    if (file.diagnostic.has_value()) {
      return response;
    }

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

    std::vector<lsDocumentHighlight> highlights; 
    if (usage != nullptr) {
      for (auto& usage_item: file.usages) {
        if (usage_item.declared_at == usage->declared_at) {
          highlights.push_back(lsDocumentHighlight{usage_item.range});          
        }
      }
    }

    response.result = std::move(highlights);

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
    
    auto& file_uri = notify.params.textDocument.uri;
    ViewedFile& file = find_file(file_uri);

    logger.log(lsp::Log::Level::INFO, "opened file with uri " + notify.params.textDocument.uri.raw_uri_);
  });

  client_endpoint.registerHandler([&](Notify_TextDocumentDidChange::notify& notify) {
    if (!initialized) {
        return;
    }

    if (notify.params.contentChanges.empty()) {
      logger.warning("didChange event without contentChanges for file " + notify.params.textDocument.uri.GetAbsolutePath().path);
      return;      
    }

    auto& file_uri = notify.params.textDocument.uri;
    ViewedFile& target_file = find_file(file_uri);

    target_file.unsaved_content = notify.params.contentChanges.back().text;
    target_file.Invalidate();
    update_diagnostics(target_file);

    for (auto& [_, file]: file_cache) {
      if (file.abs_path_ == target_file.abs_path_) {
        // Already re-evaluated, avoid unnecessary work.
        continue;
      }

      file.InvalidateOnLookup();
    }
  });

  client_endpoint.registerHandler([&](Notify_TextDocumentDidSave::notify& notify) {
    if (!initialized) {
        return;
    }

    auto& file_uri = notify.params.textDocument.uri;
    ViewedFile& target_file = find_file(file_uri);

    // Now we can read just from the fs.
    target_file.unsaved_content.reset();
  });

  client_endpoint.registerHandler([&](Notify_TextDocumentDidClose::notify& notify) {
    if (!initialized) {
        return;
    }
    logger.log(lsp::Log::Level::INFO, "closing file with uri " + notify.params.textDocument.uri.raw_uri_);
    // Editor asks what to save pending changes in the file before closing.
    close_file(notify.params.textDocument.uri);
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