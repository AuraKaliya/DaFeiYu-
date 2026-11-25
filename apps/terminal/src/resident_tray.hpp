#pragma once
#include "management/resident.hpp"

namespace dfy::ui {
// 启动由原后台实例拥有的通知区域线程，工厂返回后管道继续响应。
management::StartResidentTray resident_tray_factory(std::string definition);
}
