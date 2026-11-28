#pragma once
#include <dafeiyu/local_embedding.hpp>

namespace dfy::model {
// 由模块内部把验明来源的模型字节和同版词表交给 CPU 会话。
LocalEncoder open_embedding_session(const std::filesystem::path& runtime_dll,std::string_view model,
    std::string_view vocabulary,std::string fingerprint);
}
