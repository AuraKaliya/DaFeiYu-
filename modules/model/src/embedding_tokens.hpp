#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace dfy::model {
// 解析 BGE 中文词表，以 UTF-8 字符边界执行词片匹配。
class EmbeddingTokens {
  public:
    // 按行号装入词表，验证编码所需的特殊标记。
    explicit EmbeddingTokens(std::string_view vocabulary);
    // 依次装入 CLS、有限词片、SEP，空白文本产生空向量。
    std::vector<std::int64_t> encode(std::string_view text,std::size_t limit=512) const;

  private:
    // 按最长匹配拆分一个完整词，部分匹配失败时将整词标为未知。
    void word(std::string_view text,const std::vector<std::size_t>& ends,
        std::vector<std::int64_t>& output,std::size_t content_limit) const;
    std::map<std::string,std::int64_t,std::less<>> ids_;
    std::int64_t cls_=0, sep_=0, unknown_=0;
};
}
