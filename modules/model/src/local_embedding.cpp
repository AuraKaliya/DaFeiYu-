#include <dafeiyu/local_embedding.hpp>

#ifdef DFY_WITH_ONNX_RUNTIME
#include "embedding_session.hpp"
#include "embedding_tokens.hpp"
#include <dafeiyu/digest.hpp>
#include <dafeiyu/dynamic_library.hpp>
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#include <array>
#include <cmath>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace dfy::model {
namespace {
constexpr std::size_t dimensions=512;
constexpr auto model_sha="69a0b846f4f116b5e6aabf9546ea6754d02264f3211a13a1bd69b31b8040749a";
constexpr auto vocabulary_sha="45bbac6b341c319adc98a532532882e91a9cefc0329aa57bac9ae761c27b291c";

// 一次性从指定文件取得验证过的字节，推断时使用这份已验快照。
std::string verified(const std::filesystem::path& path,std::string_view expected,std::size_t bytes) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if (!file || file.tellg()!=static_cast<std::streamoff>(bytes)) throw std::invalid_argument("embedding: wrong file size");
    std::string content(bytes,'\0');
    file.seekg(0); file.read(content.data(),static_cast<std::streamsize>(bytes));
    if (!file || file.peek()!=std::char_traits<char>::eof() || platform::sha256(content)!=expected)
        throw std::invalid_argument("embedding: file does not match the pinned asset");
    return content;
}

struct Runtime {
    platform::DynamicLibrary library;
    Ort::Env environment{nullptr};
    // 装入实际 CPU DLL 和调用表，环境先于模块释放。
    explicit Runtime(const std::filesystem::path& dll) : library(dll) {
        const auto get=reinterpret_cast<decltype(&OrtGetApiBase)>(library.symbol("OrtGetApiBase"));
        const auto* base=get();
        const auto* api=base ? base->GetApi(ORT_API_VERSION) : nullptr;
        if (!api) throw std::runtime_error("embedding: incompatible ONNX Runtime");
        Ort::InitApi(api);
        environment=Ort::Env(ORT_LOGGING_LEVEL_ERROR,"dafeiyu-embedding");
        environment.DisableTelemetryEvents();
    }
};

// 复用同一已载入的 CPU 运行时，由 C++ 一次性初始化同步首次使用。
Ort::Env& environment(const std::filesystem::path& dll) {
    static Runtime runtime(dll);
    return runtime.environment;
}

// 限制单请求占用的 CPU 核，避免异步语义查找与主循环抢占过多资源。
Ort::SessionOptions session_options() {
    Ort::SessionOptions options;
    options.SetExecutionMode(ORT_SEQUENTIAL);
    options.SetIntraOpNumThreads(2);
    options.SetGraphOptimizationLevel(ORT_ENABLE_EXTENDED);
    return options;
}

class EmbeddingSession {
  public:
    // 先完成 CPU 调用表初始化，再构造会话选项。
    EmbeddingSession(const std::filesystem::path& dll,std::string_view graph,std::string_view vocabulary)
        : tokens_(vocabulary) {
        auto& runtime=environment(dll);
        session_=Ort::Session(runtime,graph.data(),graph.size(),session_options());
    }

    // 将一条完整文本变为归一化的句首向量，关闭时只中止这一次计算。
    // 步骤：切分词片 → 准备三个整型张量 → 运行 CPU 会话 → 核对输出形状并归一化。
    std::vector<float> encode(std::string_view text,std::stop_token stop) {
        if (stop.stop_requested() || text.size()>8192) return {};
        auto ids=tokens_.encode(text);
        if (ids.empty()) return {};
        std::vector<std::int64_t> attention(ids.size(),1), types(ids.size(),0);
        const std::array<std::int64_t,2> shape{1,static_cast<std::int64_t>(ids.size())};
        const auto cpu=Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,OrtMemTypeDefault);
        std::array<Ort::Value,3> tensors{
            Ort::Value::CreateTensor<std::int64_t>(cpu,ids.data(),ids.size(),shape.data(),shape.size()),
            Ort::Value::CreateTensor<std::int64_t>(cpu,attention.data(),attention.size(),shape.data(),shape.size()),
            Ort::Value::CreateTensor<std::int64_t>(cpu,types.data(),types.size(),shape.data(),shape.size())};
        constexpr std::array inputs{"input_ids","attention_mask","token_type_ids"};
        constexpr std::array outputs{"last_hidden_state"};
        Ort::RunOptions options;
        std::stop_callback cancel(stop,[&options]() noexcept {try {options.SetTerminate();} catch (...) {}});
        auto encoded=session_.Run(options,inputs.data(),tensors.data(),inputs.size(),outputs.data(),outputs.size());
        if (stop.stop_requested()) return {};
        const auto info=encoded.front().GetTensorTypeAndShapeInfo();
        if (info.GetElementType()!=ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
            info.GetShape()!=std::vector<std::int64_t>{1,shape[1],dimensions}) return {};
        const auto* cls=encoded.front().GetTensorData<float>();
        double squared=0;
        for (std::size_t i=0; i<dimensions; ++i) squared+=static_cast<double>(cls[i])*cls[i];
        if (!(squared>0) || !std::isfinite(squared)) return {};
        const auto norm=std::sqrt(squared);
        std::vector<float> result(dimensions);
        for (std::size_t i=0; i<dimensions; ++i) result[i]=static_cast<float>(cls[i]/norm);
        return result;
    }
  private:
    EmbeddingTokens tokens_;
    Ort::Session session_{nullptr};
};
}

// 构建可中止的单线程编码函数，推断错误由调用方按空向量处理。
LocalEncoder open_embedding_session(const std::filesystem::path& runtime_dll,std::string_view model,
    std::string_view vocabulary,std::string fingerprint) {
    auto impl=std::make_shared<EmbeddingSession>(runtime_dll,model,vocabulary);
    return {std::move(fingerprint),[impl=std::move(impl)](std::string_view text,std::stop_token stop){
        try {return impl->encode(text,stop);} catch (const std::exception&) {return std::vector<float>{};}
    }};
}

// 装配固定的图、词表和预处理版本，任一步不可用均回到普通词面匹配。
LocalEncoder open_bge_small_zh(const std::filesystem::path& model_directory,const std::filesystem::path& runtime_dll) noexcept {
    try {
        const auto vocabulary=verified(model_directory/"vocab.txt",vocabulary_sha,109540);
        const auto graph=verified(model_directory/"model.onnx",model_sha,94851877);
        return open_embedding_session(runtime_dll,graph,vocabulary,
            platform::sha256(std::string(model_sha)+"/"+vocabulary_sha+"/bert-case-v1/cls-l2-v1"));
    } catch (const std::exception&) {return {};}
}
}
#else
namespace dfy::model {
// 默认构建无需部署额外运行时，调用方直接沿用词面匹配。
LocalEncoder open_bge_small_zh(const std::filesystem::path&,const std::filesystem::path&) noexcept {return {};}
}
#endif
