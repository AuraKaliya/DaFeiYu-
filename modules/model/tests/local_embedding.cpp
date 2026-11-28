#include <dafeiyu/local_embedding.hpp>
#include <iostream>
#include <stdexcept>
#ifdef DFY_TEST_ONNX
#include "../src/embedding_session.hpp"
#include <array>
#include <cmath>
#endif

namespace {
// 固定输入暴露首次错误，测试期间只运行合成张量。
void require(bool condition,const char* detail) {
    if (!condition) throw std::runtime_error(detail);
}
#ifdef DFY_TEST_ONNX
// 编码 ONNX Protobuf 所需的长度与整数。
std::string varint(std::size_t value) {
    std::string bytes;
    for (; value>127; value>>=7) bytes.push_back(static_cast<char>((value&127)|128));
    bytes.push_back(static_cast<char>(value)); return bytes;
}
// 写一项整数型 protobuf 字段。
std::string field(unsigned number,std::size_t value) {return varint(number*8)+varint(value);}
// 写一项带长度的子字段，支持合成张量的二进制原值。
std::string field(unsigned number,std::string_view bytes) {
    return varint(number*8+2)+varint(bytes.size())+std::string(bytes);
}
// 声明测试图的张量名称、数值类型与固定形状。
std::string tensor(std::string_view name,unsigned type,std::initializer_list<unsigned> dimensions) {
    std::string shape;
    for (const auto size : dimensions) shape+=field(1,field(1,size));
    return field(1,name)+field(2,field(1,field(1,type)+field(2,shape)));
}
// 构建仅转交固定常量的 CPU 图，第二位置的值不同以区分池化方式。
std::string graph() {
    std::array<float,3*512> values{}; values[0]=3; values[1]=4; values[512]=200;
    const std::string_view floats(reinterpret_cast<const char*>(values.data()),sizeof(values));
    const auto node=field(1,"synthetic")+field(2,"last_hidden_state")+field(4,"Identity");
    const auto data=field(1,1)+field(1,3)+field(1,512)+field(2,1)+field(8,"synthetic")+field(9,floats);
    const auto body=field(1,node)+field(2,"fixed_output_fixture")+field(5,data)+
        field(11,tensor("input_ids",7,{1,3}))+field(11,tensor("attention_mask",7,{1,3}))+
        field(11,tensor("token_type_ids",7,{1,3}))+field(12,tensor("last_hidden_state",1,{1,3,512}));
    return field(1,8)+field(7,body)+field(8,field(2,13));
}
#endif
}
// 验证缺件回退，并在可选 CPU 运行时检查形状、句首池化及取消。
int main(int argc,char** argv) try {
    require(!dfy::model::open_bge_small_zh("absent-model","absent-runtime").encode,"missing model should disable encoding");
#ifdef DFY_TEST_ONNX
    require(argc==2,"runtime DLL required by the synthetic graph test");
    const auto encoder=dfy::model::open_embedding_session(std::filesystem::path(argv[1]),graph(),"[CLS]\n[SEP]\n[UNK]\n牛\n","fixture-v1");
    const auto vector=encoder.encode("牛",{});
    require(vector.size()==512 && std::abs(vector[0]-0.6F)<1e-6F && std::abs(vector[1]-0.8F)<1e-6F &&
        vector[2]==0 && encoder.fingerprint=="fixture-v1","wrong CLS vector or normalization");
    std::stop_source canceled; canceled.request_stop();
    require(encoder.encode("牛",canceled.get_token()).empty() && encoder.encode(" ",{}).empty(),"canceled or blank request produced a vector");
#else
    (void)argc; (void)argv;
#endif
    return 0;
} catch (const std::exception& error) {
    std::cerr<<error.what()<<'\n';
    return 1;
}
