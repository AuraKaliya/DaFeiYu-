#pragma once
#include <string>

namespace dfy::tests {
// v1 和 v3 样本保留各自原文与回执，只移除当时尚不存在的后续结构。
inline const std::string drop_later_memory_schema=
    "DROP VIEW readable_letter; DROP TABLE idle_schedule; DROP TABLE letter_digest; DROP TABLE case_term;"
    "DROP TABLE case_opinion; DROP TABLE case_member; DROP TABLE observation_case;"
    "DROP TABLE knowledge_grant; DROP TABLE knowledge_basis; DROP TABLE knowledge_edition; DROP TABLE knowledge_article;"
    "DROP TABLE child_letter; DROP TABLE child_note; DROP TABLE child_owner;";
}
