export type Data = Readonly<Record<string, unknown>>;
export type StreamKind = 'trace' | 'game';

export interface LineRef {
  readonly stream: StreamKind;
  readonly line: number;
  readonly start: number;
  readonly end: number;
}

export interface ScanResult {
  readonly lines: number;
  readonly state: 'closed' | 'open' | 'complete' | 'invalid';
  readonly issue?: string;
}

const maximumFile = 512 * 1024 * 1024;
const maximumLine = 16 * 1024 * 1024;
const maximumDepth = 48;

// 从日志边界收窄 JSON 对象，供不同版本的数据投影使用。
export function object(value: unknown): Data | undefined {
  if (value !== null && typeof value === 'object' && !Array.isArray(value)) return value as Data;
}

// 读取已验证的数组对象，忽略不属于对象列表的字段。
export function objects(value: unknown): Data[] {
  return Array.isArray(value) ? value.map(object).filter(entry => entry !== undefined) : [];
}

// 读取可能缺失的正文，保留实际字符串内容。
export function text(value: unknown): string | undefined {
  if (typeof value === 'string') return value;
}

// 将原生数字身份转成可精确比较的字符串。
export function id(value: unknown): string | undefined {
  if (typeof value === 'number' && Number.isSafeInteger(value) && value >= 0) return String(value);
  if (typeof value === 'string' && /^\d+$/.test(value)) return value;
}

// 与宿主一致地拒绝重复键和过深对象，防止日志身份被悄悄覆盖。
// 步骤：枚举结构字符 → 跳过字符串 → 在各自对象内核对键和深度。
function verifyShape(source: string) {
  const token = /[\[\]{}"]/g;
  const quotes = /["\\]/g;
  const containers: (Set<string> | undefined)[] = [];
  let next: RegExpExecArray | null;
  while ((next = token.exec(source)) !== null) {
    if (next[0] === '{' || next[0] === '[') {
      if (containers.length >= maximumDepth) throw new Error('记录嵌套超过限制');
      containers.push(next[0] === '{' ? new Set() : undefined);
    } else if (next[0] === '}' || next[0] === ']') {
      containers.pop();
    } else {
      quotes.lastIndex = token.lastIndex;
      let end = quotes.exec(source);
      while (end?.[0] === '\\') {
        quotes.lastIndex += 1;
        end = quotes.exec(source);
      }
      if (!end) throw new Error('记录有未闭合的字符串');
      token.lastIndex = quotes.lastIndex;
      const keys = containers.at(-1);
      if (!keys) continue;
      let separator = quotes.lastIndex;
      while (source[separator] === ' ' || source[separator] === '\t' || source[separator] === '\n' || source[separator] === '\r')
        separator += 1;
      if (source[separator] !== ':') continue;
      const name = source.slice(next.index + 1, end.index);
      const key = name.includes('\\') ? JSON.parse(source.slice(next.index, quotes.lastIndex)) as string : name;
      if (keys.has(key)) throw new Error('记录有重复字段');
      keys.add(key);
    }
  }
}

// 保留超出浏览器整数精度的原生票据和时间值。
export function parseJson(source: string): unknown {
  verifyShape(source);
  return JSON.parse(source, (_key: string, value: unknown, context?: { source?: string }) => {
    if (typeof value !== 'number' || !Number.isInteger(value) || Number.isSafeInteger(value)) return value;
    if (context?.source === undefined) throw new Error('浏览器无法读取完整的原生整数');
    return /^-?\d+$/.test(context.source) ? context.source : value;
  });
}

// 读取原生对象或宿主保留为字符串的业务正文。
export function decodedObject(value: unknown): Data | undefined {
  if (typeof value !== 'string') return object(value);
  try {
    return object(parseJson(value));
  } catch {
    return undefined;
  }
}

// 用字节区间读回原记录，为多字节中文保留准确的行锚点。
export async function readLine(file: Blob, ref: LineRef): Promise<string> {
  return (await file.slice(ref.start, ref.end).text()).replace(/\r$/, '');
}

// 拼接一条跨输入块的记录，同一块内的行直接复用原字节。
function joined(fragments: Uint8Array[], tail: Uint8Array, size: number): Uint8Array {
  if (!fragments.length) return tail;
  const row = new Uint8Array(size);
  let offset = 0;
  for (const fragment of fragments) {
    row.set(fragment, offset);
    offset += fragment.length;
  }
  row.set(tail, offset);
  return row;
}

// 校验宿主 Trace 的格式、起止记录和连续序号。
function validateTrace(row: Data, line: number, closed: boolean): string | undefined {
  if (closed) return '结束标记后还有记录';
  if (row.schema !== 1) return '无法识别的 Trace 格式版本';
  if (row.seq !== line) return '记录序号不连续';
  const kind = text(row.kind);
  if (!kind || (kind === 'trace.open') !== (line === 1)) return '缺少有效的起始记录或事件类型';
}

// 逐行交付可核对的原始记录，并保留断尾之前的完整前缀。
// 步骤：核对文件容量 → 按字节切分并校验 → 交付行索引 → 返回完整性状态。
export async function scanFile(
  file: Blob, stream: StreamKind, visit: (row: Data, ref: LineRef) => void, progress?: (bytes: number) => void,
): Promise<ScanResult> {
  if (file.size > maximumFile) return { lines: 0, state: 'invalid', issue: '文件超过 512 MiB' };
  const input = file.stream().getReader();
  const utf8 = new TextDecoder('utf-8', { fatal: true });
  const fragments: Uint8Array[] = [];
  let bytes = 0;
  let start = 0;
  let lines = 0;
  let closed = false;
  try {
    while (true) {
      const chunk = await input.read();
      if (chunk.done) break;
      let cursor = 0;
      while (cursor < chunk.value.length) {
        const newline = chunk.value.indexOf(10, cursor);
        const end = bytes + (newline < 0 ? chunk.value.length : newline);
        if (end - start > maximumLine) throw new Error('单条记录超过 16 MiB');
        if (newline < 0) {
          fragments.push(chunk.value.subarray(cursor));
          break;
        }

        const payload = joined(fragments, chunk.value.subarray(cursor, newline), end - start);
        const row = object(parseJson(utf8.decode(payload)));
        if (!row) throw new Error('记录应为 JSON 对象');
        if (stream === 'trace') {
          const problem = validateTrace(row, lines + 1, closed);
          if (problem) throw new Error(problem);
          closed = row.kind === 'trace.closed';
        }
        visit(row, { stream, line: ++lines, start, end });
        fragments.length = 0;
        start = end + 1;
        cursor = newline + 1;
      }
      bytes += chunk.value.length;
      progress?.(bytes);
    }
  } catch (error) {
    return { lines, state: 'invalid', issue: `第 ${lines + 1} 行 · ${error instanceof Error ? error.message : '读取失败'}` };
  } finally {
    await input.cancel().catch(() => undefined);
    input.releaseLock();
  }
  if (fragments.length) return { lines, state: 'open', issue: `第 ${lines + 1} 行尚未结束` };
  if (!lines && stream === 'trace') return { lines, state: 'invalid', issue: 'Trace 文件为空' };
  return { lines, state: stream === 'trace' ? (closed ? 'closed' : 'open') : 'complete' };
}
