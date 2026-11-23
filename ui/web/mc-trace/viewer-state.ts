import { onScopeDispose, ref, shallowRef } from 'vue';
import { object, parseJson, readLine } from './trace-file.ts';
import type { LineRef, StreamKind } from './trace-file.ts';
import type { TraceFiles, WorkerUpdate } from './trace.worker.ts';
import type { TraceDocument } from './mc-links.ts';

export interface LoadedTrace extends TraceFiles {
  readonly index: TraceDocument;
}

export interface ImportState {
  readonly file: string;
  readonly stream: StreamKind;
  readonly fraction: number;
}

// 管理本地文件快照，完整替换索引并隔离已取消的后台导入。
// 步骤：创建读取状态 → 接收当前工作结果 → 按原文件读回记录。
export function useTraceFiles() {
  const loaded = shallowRef<LoadedTrace>();
  const importing = shallowRef<ImportState>();
  const error = ref('');
  let worker: Worker | undefined;

  // 取消在途读取并丢弃其未来消息。
  const cancel = () => {
    worker?.terminate();
    worker = undefined;
    importing.value = undefined;
  };
  onScopeDispose(cancel);

  // 打开新文件或更新同一宿主快照的游戏回执。
  // 步骤：关闭旧读取 → 启动新索引 → 返回进度、成品或错误。
  function open(files: TraceFiles) {
    cancel();
    error.value = '';
    importing.value = { file: files.trace.name, stream: 'trace', fraction: 0 };

    try {
      const current = new Worker(new URL('./trace.worker.ts', import.meta.url), { type: 'module' });
      worker = current;
      current.onerror = () => {
        if (worker !== current) return;
        error.value = '无法启动后台文件读取。请用本地地址重新打开页面。';
        cancel();
      };
      current.onmessage = ({ data }: MessageEvent<WorkerUpdate>) => {
        if (worker !== current) return;
        if (data.type === 'progress') {
          const file = data.stream === 'game' ? files.game : files.trace;
          if (file) importing.value = { file: file.name, stream: data.stream,
            fraction: file.size ? Math.min(data.bytes / file.size, 1) : 0 };
          return;
        }
        if (data.type === 'indexed') loaded.value = { ...files, index: data.index };
        else error.value = data.message;
        cancel();
      };
      current.postMessage(files);
    } catch (failure) {
      error.value = failure instanceof Error ? failure.message : '无法读取本地文件';
      cancel();
    }
  }

  return { loaded, importing, error, open, cancel };
}

// 从当前宿主或游戏文件取回同一条原始记录，交由调用者决定展示粒度。
export async function loadRecord(source: LoadedTrace, ref: LineRef) {
  const file = ref.stream === 'trace' ? source.trace : source.game;
  if (!file) throw new Error('原文件已关闭');
  const raw = await readLine(file, ref);
  return { raw, data: object(parseJson(raw)) };
}
