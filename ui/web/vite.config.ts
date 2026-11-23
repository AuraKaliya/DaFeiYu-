import { defineConfig } from 'vite';
import vue from '@vitejs/plugin-vue';
import { fileURLToPath } from 'node:url';
import licenseNotices from './licenses.ts';

// 预编译主界面与独立 Frame，所有产物随本地目录发布，不依赖开发服务器。
export default defineConfig({
  plugins: [vue(), licenseNotices()], base: './', publicDir: false,
  build: {
    target: 'es2022', emptyOutDir: false,
    rollupOptions: { input: {
      main: fileURLToPath(new URL('./index.html', import.meta.url)),
      panel: fileURLToPath(new URL('./main-panel.html', import.meta.url)),
      frame: fileURLToPath(new URL('./frames/index.html', import.meta.url)),
    } },
  },
});
