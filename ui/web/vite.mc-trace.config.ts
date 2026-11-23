import { fileURLToPath } from 'node:url';
import vue from '@vitejs/plugin-vue';
import { defineConfig } from 'vite';
import licenseNotices from './licenses.ts';

// 单独构建 MC Trace 查看器，并通过回环地址提供本地资源。
export default defineConfig({
  root: fileURLToPath(new URL('./mc-trace', import.meta.url)),
  plugins: [vue(), licenseNotices()],
  base: './',
  publicDir: false,
  build: {
    target: 'es2022',
    outDir: '../dist-mc-trace',
    emptyOutDir: true,
  },
  preview: { host: '127.0.0.1', port: 4174, strictPort: true },
});
