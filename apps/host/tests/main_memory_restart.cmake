# 每个阶段启动独立进程，共用此轮专属的档案目录。
string(RANDOM LENGTH 16 run_id)
foreach(phase IN ITEMS save reopen revoked)
  execute_process(COMMAND "${PROGRAM}" "${ROOT}/${run_id}" "${phase}" COMMAND_ERROR_IS_FATAL ANY)
endforeach()
