import { readFileSync, readdirSync } from 'node:fs';
import { resolve } from 'node:path';
import type { Plugin } from 'vite';

// 将实际进入页面的依赖原始许可证随静态资产发布，不收集用户目录内容。
function licenseNotices(): Plugin {
  return {
    name: 'dafeiyu-license-notices',
    // 按构建模块确定依赖根目录，缺失许可证使构建失败而不是静默省略。
    // 步骤：收集依赖包 → 校验包元数据 → 原样输出许可证与清单。
    generateBundle() {
      const roots = new Set([resolve('node_modules/vite')]);
      for (const module of this.getModuleIds()) {
        const path = module.replaceAll('\\', '/');
        const offset = path.lastIndexOf('/node_modules/');
        if (offset < 0) continue;
        const parts = path.slice(offset + 14).split('/');
        roots.add(path.slice(0, offset + 14) + parts.slice(0, parts[0].startsWith('@') ? 2 : 1).join('/'));
      }
      const packages: { name: string; version: string }[] = [];
      for (const root of [...roots].sort()) {
        const metadata: unknown = JSON.parse(readFileSync(resolve(root, 'package.json'), 'utf8'));
        if (!metadata || typeof metadata !== 'object' || !('name' in metadata) || typeof metadata.name !== 'string' ||
            !('version' in metadata) || typeof metadata.version !== 'string') throw new Error('Invalid UI dependency metadata');
        const files = readdirSync(root).filter(name => /^(licen[cs]e|copying|notice)(\..*)?$/i.test(name));
        if (!files.length) throw new Error('Missing dependency license: ' + metadata.name);
        packages.push({ name: metadata.name, version: metadata.version });
        for (const file of files) this.emitFile({ type: 'asset',
          fileName: 'licenses/' + metadata.name.replaceAll('/', '_') + '/' + file,
          source: readFileSync(resolve(root, file)),
        });
      }
      this.emitFile({ type: 'asset', fileName: 'licenses/packages.json', source: JSON.stringify(packages, null, 2) });
    },
  };
}
export default licenseNotices;
