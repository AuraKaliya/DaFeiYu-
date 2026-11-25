# 验证新事件链路的顺序接纳、有界引用及不可变载荷寿命。
dfy_add_unit_test(events.store dfy_event_store_tests event_store.cpp dfy_event_streams)

# 验证独立订阅队列、两种关闭及读出事件的持有责任。
dfy_add_unit_test(events.subscription_queue dfy_subscription_queue_tests subscription_queue.cpp dfy_event_streams)

# 验证广播接纳的原子性、接收方隔离及事件引用预算。
dfy_add_unit_test(events.broadcast dfy_broadcast_channel_tests broadcast_channel.cpp dfy_event_streams)

# 验证信箱的定向投递、地址撤销和数据持有预算。
dfy_test_executable(dfy_mailboxes_tests mailboxes.cpp)
target_link_libraries(dfy_mailboxes_tests PRIVATE dfy_event_streams dfy_build_options)
add_test(NAME events.mailboxes COMMAND dfy_mailboxes_tests)

# 验证有界读取请求、取消与流结束区分及生产 FIFO 完成通知。
dfy_add_unit_test(events.subscription_reader dfy_subscription_reader_tests subscription_reader.cpp dfy_event_streams)

# 事件测试直接链接独立总线，不引入模型或平台传输。
dfy_add_unit_test(events.bus dfy_events_tests events.cpp dfy_events)

# 验证消费者组的有界成员、共享领取和独立关闭语义。
dfy_add_unit_test(events.consumer_group dfy_consumer_group_tests consumer_group.cpp dfy_event_streams)

# 验证组成员等待、取消、退出和保留结果的独立预算。
dfy_add_unit_test(events.group_reader dfy_group_reader_tests group_reader.cpp dfy_event_streams)

# 验证原事件处理跨重试保持顺序、身份及实际退出边界。
dfy_add_unit_test(events.retry_processing dfy_retry_processing_tests retry_processing.cpp dfy_event_streams)

# 验证观察失败隔离和控制边界的许可、期限及实际退出。
dfy_add_unit_test(events.hooks dfy_hooks_tests hooks.cpp dfy_event_streams)

# 验证真实事件共享存储及跨处理队列的所有者预算。
dfy_add_unit_test(events.budget dfy_event_budget_tests event_budget.cpp dfy_event_streams)

# 验证真实事件处理的业务结果、实际退出和独立数据寿命。
dfy_add_unit_test(events.processing dfy_processing_tests processing.cpp dfy_event_streams)
