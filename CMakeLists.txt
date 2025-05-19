cmake_minimum_required(VERSION 3.14 FATAL_ERROR)

project(etude-lsp-server)
set(CMAKE_CXX_STANDARD 20)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(BUILD_SHARED_LIBS OFF) # So that it is portable across linuxes.
    if(WIN32 AND MSVC)
        set(CMAKE_FIND_LIBRARY_SUFFIXES .lib)
    else()
        set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
        add_compile_options(-static)
        add_link_options(-static)
    endif()
else()
    add_compile_options(-fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

# add_compile_options(-fsanitize=thread)
# add_link_options(-fsanitize=thread)

include(cmake/CPM.cmake)

add_executable(server src/server.cpp src/lsp_visitor.cpp)

CPMAddPackage("gh:valeriy-zainullin/LspCpp-tmp-fork#master")
# Так вот почему санитайзеры ругаются. Комплиятор уже при сборке говорит о том,
#   что могут быть проблемы. Если их добавить в сборку, то выйдут ошибки компиляции
#   на эту библиотеку. Эту ошибку в runtime можно избежать, если знать, как заставить эту
#   сетевую библиотеку перезаписать неинициализированные поля. Постараемся это
#   сделать.. Баги, баги, никто не собирал с санитайзерами.
target_compile_options(network-uri PRIVATE "-Wno-error=maybe-uninitialized")
target_link_libraries(server PRIVATE lspcpp)

add_subdirectory(etude)
# CPMAddPackage("gh:otakubeam/etude#effcece34dacc2e20d5db81c276b0e22ae984814")
target_link_libraries(server PRIVATE compiler)

target_compile_definitions(server PRIVATE TRACE_VISITOR=0)

