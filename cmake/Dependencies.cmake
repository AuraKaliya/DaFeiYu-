include(FetchContent)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install ON CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(sqlite_source
  URL https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip
  URL_HASH SHA256=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json sqlite_source)
add_library(dfy_sqlite STATIC ${sqlite_source_SOURCE_DIR}/sqlite3.c)
target_include_directories(dfy_sqlite PUBLIC ${sqlite_source_SOURCE_DIR})
target_compile_definitions(dfy_sqlite PRIVATE SQLITE_THREADSAFE=1 SQLITE_DQS=0)
find_package(Threads REQUIRED)
target_link_libraries(dfy_sqlite PRIVATE Threads::Threads ${CMAKE_DL_LIBS})

install(FILES ${nlohmann_json_SOURCE_DIR}/LICENSE.MIT DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses RENAME nlohmann-json-LICENSE.MIT)
