import { indexHost } from './rounds.ts';
import type { HostIndex, Round } from './rounds.ts';
import { indexGame, projectMc } from './mc-records.ts';
import type { McRecord } from './mc-records.ts';
import { id, text } from './trace-file.ts';
import type { LineRef, ScanResult, StreamKind } from './trace-file.ts';

export interface RoundLink {
  readonly round: string;
  readonly label: string;
  readonly ref?: LineRef;
}

export interface RoundEvidence {
  readonly entries: readonly McRecord[];
  readonly references: readonly McRecord[];
  readonly links: readonly RoundLink[];
}

export interface TraceDocument extends HostIndex {
  readonly evidence: ReadonlyMap<string, RoundEvidence>;
  readonly game?: ScanResult;
  readonly gameRecords: readonly McRecord[];
  readonly unlinked: readonly McRecord[];
}

type Owners = Map<string, Round | null>;

interface EvidenceDraft {
  readonly entries: McRecord[];
  readonly references: Set<McRecord>;
  readonly links: Map<string, RoundLink>;
}

// 把身份限制在确认过的命名空间，避免跨世界复用编号时被合并。
function scoped(scope?: string, key?: string) {
  if (scope && key && key !== '0') return JSON.stringify([scope, key]);
}

// 遇到多个可信拥有者时显式保留歧义，不按出现顺序取一个。
function remember(index: Owners, key: string | undefined, round: Round) {
  if (!key) return;
  const prior = index.get(key);
  index.set(key, prior === undefined || prior === round ? round : null);
}

// 只返回唯一且与已知世界相容的原回合。
function unique(index: Owners, key?: string, generation?: string): Round | undefined {
  const round = key ? index.get(key) : undefined;
  if (round && (!generation || !round.input?.generation || round.input.generation === generation)) return round;
}

// 按宿主和领域身份关联证据，不用日志到达顺序推断因果。
// 步骤：索引原接纳与原话 → 固定计划和游戏工作归属 → 连接读回结果及后继回合。
function attachEvidence(rounds: readonly Round[], records: readonly McRecord[]) {
  const request: Owners = new Map();
  const invocation: Owners = new Map();
  const message: Owners = new Map();
  const work: Owners = new Map();
  const asks: Owners = new Map();
  const delegations: Owners = new Map();
  const cards = new Map<string, EvidenceDraft>();
  const byWork = new Map<string, McRecord[]>();
  const unlinked: McRecord[] = [];
  for (const round of rounds) {
    cards.set(round.key, { entries: [], references: new Set(), links: new Map() });
    remember(request, scoped(round.session, round.accepted?.request), round);
    for (const event of round.events) if (event.kind === 'tool.admitted' && event.invocation !== '0')
      remember(invocation, event.invocation, round);
    const input = round.input;
    if (input?.trigger.code === 'game.event')
      for (const fact of input.facts) remember(message, scoped(input.generation, fact.sequence), round);
    if (input?.trigger.code === 'main.delegation')
      remember(delegations, scoped(input.generation, id(input.related.delegation)), round);
  }

  // 用正式回合、接纳调用或原生事件定位领域记录的拥有者。
  const directOwner = (row: McRecord) => {
    const generation = row.generation;
    return unique(request, scoped(row.session, row.request), generation) ??
      unique(invocation, row.invocation ?? row.plan ?? row.item, generation) ??
      (row.kind === 'event' ? unique(message, scoped(generation, row.order)) : undefined) ??
      unique(asks, row.ask, generation) ??
      unique(delegations, scoped(generation, row.delegation));
  };

  for (const row of records) {
    if (row.code !== 'ask.opened' || !row.ask) continue;
    const owner = directOwner(row);
    if (owner) remember(asks, row.ask, owner);
  }
  for (const row of records) {
    const owner = directOwner(row);
    const scope = row.generation ?? owner?.input?.generation;
    if (!owner || row.kind === 'turn') continue;
    for (const key of [row.work, row.follow, ...row.workItems]) remember(work, scoped(scope, key), owner);
  }

  // 增加可反查的原回合链接，原证据仍归自己被接纳的回合。
  const link = (from: Round, to: Round | undefined, label: string, ref?: LineRef) => {
    if (!to || from === to) return;
    cards.get(from.key)?.links.set(`${to.key}/${label}`, { round: to.key, label, ref });
    cards.get(to.key)?.links.set(`${from.key}/后继回合`, { round: from.key, label: '后继回合' });
  };
  for (const row of records) {
    const anchored = directOwner(row);
    const scope = row.generation ?? anchored?.input?.generation;
    const owner = anchored ?? unique(work, scoped(scope, row.work ?? row.follow));
    if (!owner) {
      unlinked.push(row);
      continue;
    }
    cards.get(owner.key)?.entries.push(row);
    for (const workKey of new Set([scoped(scope, row.work), scoped(scope, row.follow)])) {
      if (!workKey) continue;
      const list = byWork.get(workKey) ?? [];
      if (!list.length) byWork.set(workKey, list);
      list.push(row);
    }
    if (row.kind === 'question') {
      const answer = unique(message, scoped(scope, row.answerOrder));
      if (answer && answer !== owner) {
        cards.get(answer.key)?.references.add(row);
        link(answer, owner, '原追问', row.ref);
      }
    }
  }
  for (const round of rounds) {
    const input = round.input;
    const facts = input?.related;
    if (!input || !facts) continue;
    const scope = input.generation;
    const card = cards.get(round.key);
    link(round, unique(message, scoped(scope, id(facts.origin))), '原委托');
    link(round, unique(invocation, id(facts.question), scope), '原追问');
    link(round, unique(invocation, id(facts.approval), scope), '原审批');
    link(round, unique(invocation, id(facts.reviewed), scope), '被复核的工作');
    for (const plan of Array.isArray(facts.plans) ? facts.plans : [])
      link(round, unique(invocation, id(plan), scope), '原计划');
    const workIds = Array.isArray(facts.works) ? [...facts.works] : [];
    if (input.trigger.code === 'follow.attention') workIds.push(facts.follow);
    for (const value of workIds) {
      const key = scoped(scope, text(value));
      if (!key) continue;
      const owner = unique(work, key, scope);
      link(round, owner, '原游戏工作');
      if (owner && owner !== round) {
        for (const row of byWork.get(key) ?? []) {
          if (row.kind !== 'attention' || row.episode === id(facts.episode)) card?.references.add(row);
        }
      }
    }
  }
  const evidence = new Map<string, RoundEvidence>();
  for (const [key, card] of cards) {
    evidence.set(key, { entries: card.entries, references: [...card.references], links: [...card.links.values()] });
  }
  return { evidence, unlinked };
}

// 构建本局的宿主、桥接与跨页证据索引，保留无法安全归属的原记录。
export async function indexDocument(
  trace: Blob, game?: Blob, progress?: (stream: StreamKind, bytes: number) => void,
): Promise<TraceDocument> {
  const records: McRecord[] = [];
  const host = await indexHost(trace, (row, _ref, entry) => {
    const projected = projectMc(row, entry);
    if (projected) records.push(projected);
  }, bytes => progress?.('trace', bytes));
  const side = game ? await indexGame(game, bytes => progress?.('game', bytes)) : undefined;
  const gameRecords = side?.records ?? [];
  const joined = attachEvidence(host.rounds, [...records, ...gameRecords]);
  return { ...host, ...joined, game: side?.source, gameRecords };
}
