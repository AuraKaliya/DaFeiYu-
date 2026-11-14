# 只在明确构建 GUI 时取得固定版本 SDK，不把浏览器运行库或 Node 加入 CLI 依赖。
FetchContent_Declare(dfy_webview2_sdk
  URL https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.4314.35/microsoft.web.webview2.1.0.4314.35.nupkg
  DOWNLOAD_NAME webview2.zip
  URL_HASH SHA256=90778deb1b77e36292828b4b5833c9255c27533f40e0ddd55891ae718944edc4
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(dfy_webview2_sdk)

add_library(dfy_webview2 INTERFACE)
target_include_directories(dfy_webview2 SYSTEM INTERFACE "${dfy_webview2_sdk_SOURCE_DIR}/build/native/include")
target_link_libraries(dfy_webview2 INTERFACE
  "${dfy_webview2_sdk_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib" version shlwapi)

# 保留 SDK 原始许可证及第三方声明，Evergreen Runtime 仍由系统独立安装。
install(FILES "${dfy_webview2_sdk_SOURCE_DIR}/LICENSE.txt" DESTINATION licenses RENAME WebView2-SDK-LICENSE.txt CONFIGURATIONS Release)
install(FILES "${dfy_webview2_sdk_SOURCE_DIR}/NOTICE.txt" DESTINATION licenses RENAME WebView2-SDK-NOTICE.txt CONFIGURATIONS Release)
