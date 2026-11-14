#pragma once
#include <dafeiyu/session.hpp>
namespace dfy::agent {
// 过渡头只保留类型名称兼容，正式会话与异步会话使用同一个实现。
using AsyncSession = Session;
}