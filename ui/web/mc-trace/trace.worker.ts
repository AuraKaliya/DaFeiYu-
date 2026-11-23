import { indexDocument } from './mc-links.ts';
import type { TraceDocument } from './mc-links.ts';
import type { StreamKind } from './trace-file.ts';

export interface TraceFiles {
  readonly trace: File;
  readonly game?: File;
}

export type WorkerUpdate =
  | { type: 'progress'; stream: StreamKind; bytes: number }
  | { type: 'indexed'; index: TraceDocument }
  | { type: 'failed'; message: string };

const port = self as DedicatedWorkerGlobalScope;

// 在独立线程构建索引，分批报告读取进度并返回可克隆的索引。
// 步骤：逐流扫描 → 限流发送进度 → 返回结果或读取错误。
port.onmessage = async ({ data }: MessageEvent<TraceFiles>) => {
  try {
    let previous: StreamKind | undefined;
    let reported = 0;
    const index = await indexDocument(data.trace, data.game, (stream, bytes) => {
      if (stream === previous && bytes - reported < 1024 * 1024) return;
      previous = stream;
      reported = bytes;
      port.postMessage({ type: 'progress', stream, bytes } satisfies WorkerUpdate);
    });
    port.postMessage({ type: 'indexed', index } satisfies WorkerUpdate);
  } catch (error) {
    const message = error instanceof Error ? error.message : '索引失败';
    port.postMessage({ type: 'failed', message } satisfies WorkerUpdate);
  }
};
