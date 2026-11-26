add_library(dfy_agent STATIC
  src/action_observations.cpp src/confirmation.cpp src/context_capacity.cpp src/context_status.cpp
  src/diagnostic_fields.hpp src/diagnostic_json.cpp src/diagnostic_snapshot.cpp
  src/execution_policy.cpp src/observation.cpp src/observation_controller.cpp src/observation_inbox.cpp
  src/observation_input.cpp src/observation_trigger.cpp src/observation_verification.cpp
  src/plan_book.cpp src/plan_definition.cpp src/plan_protocol.cpp src/run_context.cpp
  src/prompt_rules.cpp src/run_journal.cpp src/run_loop.cpp src/run_trace.cpp src/session.cpp src/timeline_review.cpp
  src/tool_batch.cpp src/tool_catalog.cpp src/tool_executor.cpp src/tool_schema.cpp src/tool_work.cpp
  src/trace_identity.cpp
  include/dafeiyu/action_observations.hpp include/dafeiyu/async_session.hpp
  include/dafeiyu/confirmation.hpp include/dafeiyu/context_capacity.hpp
  include/dafeiyu/context_status.hpp include/dafeiyu/diagnostic_json.hpp
  include/dafeiyu/diagnostic_snapshot.hpp include/dafeiyu/execution_policy.hpp
  include/dafeiyu/observation.hpp include/dafeiyu/observation_controller.hpp
  include/dafeiyu/observation_inbox.hpp include/dafeiyu/observation_input.hpp
  include/dafeiyu/observation_rebind.hpp include/dafeiyu/observation_trigger.hpp
  include/dafeiyu/observation_verification.hpp include/dafeiyu/plan_book.hpp
  include/dafeiyu/plan_definition.hpp include/dafeiyu/plan_feedback.hpp
  include/dafeiyu/plan_protocol.hpp include/dafeiyu/prompt_rules.hpp include/dafeiyu/run_capture.hpp include/dafeiyu/run_context.hpp
  include/dafeiyu/run_identity.hpp include/dafeiyu/run_journal.hpp include/dafeiyu/run_loop.hpp
  include/dafeiyu/run_options.hpp include/dafeiyu/run_trace.hpp include/dafeiyu/session.hpp
  include/dafeiyu/tool_batch.hpp include/dafeiyu/tool_catalog.hpp include/dafeiyu/tool_executor.hpp
  include/dafeiyu/tool_schema.hpp include/dafeiyu/tool_work.hpp include/dafeiyu/trace_identity.hpp
  include/dafeiyu/turn_input.hpp)
target_include_directories(dfy_agent PUBLIC include)
target_link_libraries(dfy_agent PUBLIC dfy_event_streams dfy_model
  PRIVATE dfy_digest nlohmann_json::nlohmann_json dfy_build_options)
dfy_group_target(dfy_agent Agent)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
