#pragma once
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace dfy::model {
// 一个本机编码实例由单个调用线程持有，指纹标识可复用的向量空间。
struct LocalEncoder {
    std::string fingerprint;
    // 返回单位向量，空结果表示编码未完成或输入不可用。
    std::function<std::vector<float>(std::string_view,std::stop_token)> encode;
};
// 从固定版本的 BGE 中文模型与词表打开 CPU 编码器，依赖缺失时返回空实例。
LocalEncoder open_bge_small_zh(const std::filesystem::path& model_directory,const std::filesystem::path& runtime_dll) noexcept;
}
