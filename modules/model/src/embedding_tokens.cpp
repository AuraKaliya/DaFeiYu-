#include "embedding_tokens.hpp"
#include <dafeiyu/utf8.hpp>
#include <icu.h>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace dfy::model {
namespace {
// 固定中文 BERT 词表约定的汉字块，其他文字以空白及标点切分。
bool ideograph(UChar32 value) {
    constexpr std::pair<UChar32,UChar32> blocks[]={
        {0x3400,0x4dbf},{0x4e00,0x9fff},{0xf900,0xfaff},{0x20000,0x2a6df},
        {0x2a700,0x2b73f},{0x2b740,0x2b81f},{0x2b920,0x2ceaf},{0x2f800,0x2fa1f}};
    return std::ranges::any_of(blocks,[&](const auto& block){return block.first<=value && value<=block.second;});
}
}

// 按原行号建立词片 ID，保留英文字母大小写与重音。
EmbeddingTokens::EmbeddingTokens(std::string_view vocabulary) {
    if (!runtime::valid_utf8(vocabulary)) throw std::invalid_argument("embedding: invalid vocabulary encoding");
    for (std::int64_t id=0; !vocabulary.empty(); ++id) {
        const auto end=vocabulary.find('\n'); auto token=vocabulary.substr(0,end);
        if (token.ends_with('\r')) token.remove_suffix(1);
        if (token.empty() || !ids_.emplace(std::string(token),id).second)
            throw std::invalid_argument("embedding: invalid vocabulary entry");
        if (end==vocabulary.npos) break;
        vocabulary.remove_prefix(end+1);
    }
    cls_=ids_.at("[CLS]"); sep_=ids_.at("[SEP]"); unknown_=ids_.at("[UNK]");
}

// 贪心选择当前位置最长的词片，同时保留整词失败时的回滚位置。
void EmbeddingTokens::word(std::string_view text,const std::vector<std::size_t>& ends,
    std::vector<std::int64_t>& output,std::size_t content_limit) const {
    if (text.empty() || output.size()>=content_limit) return;
    const auto before=output.size(), characters=ends.size()-1;
    if (characters>100) {output.push_back(unknown_); return;}
    for (std::size_t start=0; start<characters;) {
        auto match=ids_.end();
        auto end=characters;
        for (; end>start; --end) {
            const auto part=text.substr(ends[start],ends[end]-ends[start]);
            match=start==0 ? ids_.find(part) : ids_.find(std::string("##").append(part));
            if (match!=ids_.end()) break;
        }
        if (match==ids_.end()) {output.resize(before); output.push_back(unknown_); return;}
        if (output.size()<content_limit) output.push_back(match->second);
        start=end;
    }
}

// 在 Unicode 字符边界清理无效语料、切开标点和汉字，并为结束标记预留位置。
// 步骤：校验窗口 → 扫描完整标量 → 装入原词及词片 → 追加结束标记。
std::vector<std::int64_t> EmbeddingTokens::encode(std::string_view text,std::size_t limit) const {
    if (limit<3 || limit>512 || text.size()>static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
        throw std::invalid_argument("embedding: invalid input size");
    std::vector<std::int64_t> ids{cls_};
    std::string word_text;
    std::vector<std::size_t> ends{0};
    const auto length=static_cast<std::int32_t>(text.size());
    const auto* bytes=reinterpret_cast<const std::uint8_t*>(text.data());
    for (std::int32_t i=0; i<length && ids.size()<limit-1;) {
        const auto start=i; UChar32 value;
        U8_NEXT(bytes,i,length,value);
        if (value<0) throw std::invalid_argument("embedding: invalid input encoding");
        const auto category=U_MASK(u_charType(value));
        if (value==0xfffd || ((category&U_GC_C_MASK) && value!='\t' && value!='\n' && value!='\r')) continue;
        const bool whitespace=u_isUWhiteSpace(value)!=0;
        const bool ascii_punctuation=(value>=33 && value<=47) || (value>=58 && value<=64) ||
            (value>=91 && value<=96) || (value>=123 && value<=126);
        const bool separated=ascii_punctuation || (category&U_GC_P_MASK) || ideograph(value);
        const auto scalar=text.substr(start,i-start);
        if (whitespace || separated) {
            word(word_text,ends,ids,limit-1); word_text.clear(); ends.resize(1);
            if (separated && ids.size()<limit-1) {
                const auto found=ids_.find(scalar);
                ids.push_back(found==ids_.end() ? unknown_ : found->second);
            }
        } else if (ends.size()<=101) {
            word_text.append(scalar); ends.push_back(word_text.size());
        }
    }
    word(word_text,ends,ids,limit-1);
    if (ids.size()==1) return {};
    ids.push_back(sep_);
    return ids;
}
}
