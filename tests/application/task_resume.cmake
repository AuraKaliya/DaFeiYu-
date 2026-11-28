# 在独立进程结算原任务，等数据库关闭后给所有对照复制同一快照。
string(RANDOM LENGTH 16 run_id)
set(run "${ROOT}/${run_id}")
file(MAKE_DIRECTORY "${run}")

# 按同一入口启动一个阶段，直接保留实收请求的计量输出。
function(run_phase phase)
  execute_process(COMMAND "${PROGRAM}" "${PANEL}" "${TOOLS}" "${CORPUS}" "${run}" "--resume-${phase}"
    COMMAND_ERROR_IS_FATAL ANY)
endfunction()

run_phase(seed)
set(database main-conversation.sqlite3)
file(SHA256 "${run}/seed/${database}" snapshot)
message(STATUS "T03 snapshot SHA256 ${snapshot}")
foreach(phase IN ITEMS baseline first same-name foreign revoked room live-idle live-focus live-unknown live-foreign live-revoked)
  file(MAKE_DIRECTORY "${run}/${phase}")
  file(COPY_FILE "${run}/seed/${database}" "${run}/${phase}/${database}")
  file(SHA256 "${run}/${phase}/${database}" copied)
  if(NOT copied STREQUAL snapshot)
    message(FATAL_ERROR "${phase} received a different SQLite snapshot")
  endif()
  run_phase("${phase}")
endforeach()
