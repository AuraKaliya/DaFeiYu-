#include "../src/embedding_tokens.hpp"
#include <iostream>
#include <stdexcept>

namespace {
// 以固定词表核对混合中文、罕见字符和完整词片，报出第一个偏差。
void require(bool condition,const char* label) {
    if (!condition) throw std::runtime_error(label);
}
}
// 用手写的小词表检验原始词片 ID，无需加载模型或访问网络。
int main() try {
    using Ids=std::vector<std::int64_t>;
    const dfy::model::EmbeddingTokens tokens("[PAD]\r\n[UNK]\n[CLS]\n[SEP]\n牛\n奶\n，\nPlay\n##ing\nplay\n𠀀\né\n");
    require(tokens.encode("牛奶，Playing\t𠀀 é")==Ids{2,4,5,6,7,8,10,11,3},"broken Chinese or WordPiece boundaries");
    require(tokens.encode("play Play plays")==Ids{2,9,7,1,3},"changed case or retained a partially matched word");
    require(tokens.encode("牛奶",3)==Ids{2,4,3},"missing reserved separator token");
    require(tokens.encode("Playz",3)==Ids{2,1,3},"truncated unknown word before resolving the full word");
    require(tokens.encode(std::string(101,'x'))==Ids{2,1,3},"incorrect long-word rule");
    require(tokens.encode("牛\u200b奶")==Ids{2,4,5,3},"control character not cleaned");
    require(tokens.encode("\t\n \r").empty(),"blank text must skip inference");
    return 0;
} catch (const std::exception& error) {
    std::cerr<<error.what()<<'\n';
    return 1;
}
