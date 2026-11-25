# 本工作区的多个配置共用前端依赖，安装期间其他构建也要等到依赖可用。
file(LOCK "${LOCK_FILE}" TIMEOUT 300)
set(modules "${WEB_DIR}/node_modules")
if(NOT EXISTS "${modules}/.bin/vue-tsc.cmd" OR NOT EXISTS "${modules}/.bin/vite.cmd" OR
   NOT EXISTS "${modules}/.package-lock.json" OR
   "${WEB_DIR}/package-lock.json" IS_NEWER_THAN "${modules}/.package-lock.json" OR
   "${WEB_DIR}/package.json" IS_NEWER_THAN "${modules}/.package-lock.json")
  execute_process(COMMAND "${NPM}" ci --no-audit --no-fund WORKING_DIRECTORY "${WEB_DIR}" COMMAND_ERROR_IS_FATAL ANY)
endif()
execute_process(COMMAND "${NPM}" run build -- --outDir "${ASSET_DIR}" --emptyOutDir
  WORKING_DIRECTORY "${WEB_DIR}" COMMAND_ERROR_IS_FATAL ANY)
