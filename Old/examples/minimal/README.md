# 最小架构示例

这是一套使用公共 SDK 的确定性组合，只有 model_scripted、strategy_default、channel_simulator、memory_minimal 和 action_add 五个角色。它们分别构建为独立模块，作为一个示例组合交付，不代表五项正式产品能力。

模型只按配置产生动作与最终候选；渠道只保存模拟回执；记忆只保存初始版本、读取和验证快照；动作只做有界加法。默认策略使用 SDK RunView，不依赖核心存储字段。可选 verify 参数用于验证同一决策流程中的结束复核。

最小 profile 仅授予 action.add 的 read / instance 范围。对原入站的模拟回复通过独立交付检查执行。配置没有活动、业务唤醒、记忆写入或后台 Job；只有显式输入及其恢复工作会运行。

用 DFY_BUILD_EXAMPLES=ON 构建并安装示例。BUILD_TESTING 单独启用时，这些角色只作为测试基线编译到测试目录，不随核心安装。

网络、延迟、未知效果、平台启动输入和可写记忆等场景由 tests/fixtures 提供；不要把测试开关或业务脚本语法加回本目录。
