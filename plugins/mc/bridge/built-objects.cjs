// 将同一游戏与维度内的实际放置保留为可独立于任务使用的对象。
class BuiltObjects {
  #binding;
  #cells = new Map();
  #supports = new Map();

  // 切换世界或维度时移除旧空间的约束。
  bind(generation, dimension) {
    const binding = JSON.stringify([generation, dimension]);
    if (binding === this.#binding) return;
    this.#binding = binding;
    this.#cells.clear();
    this.#supports.clear();
  }

  // 保存一个经游戏读回的对象，各格共享同一组支撑关系。
  placed(cells) {
    const keys = cells.map(cell => cellKey(cell));
    const supports = new Set(cells.map(cell => cellKey({ ...cell, y: cell.y - 1 }))
      .filter(key => !keys.includes(key)));
    const object = { keys, supports };
    for (const key of keys) {
      this.#forget(key);
      this.#cells.set(key, object);
    }
    for (const key of supports) {
      if (!this.#supports.has(key)) this.#supports.set(key, new Set());
      this.#supports.get(key).add(object);
    }
  }

  // 对选定目标及其伴随变更统一核对对象和支撑。
  permits(blocks, approved) {
    const requested = this.#cells.get(cellKey(blocks[0].position));
    return blocks.every(({ position }) => {
      const key = cellKey(position);
      const object = this.#cells.get(key);
      if (object && (!approved || object !== requested)) return false;
      return !this.#supports.has(key);
    });
  }

  // 只撤销游戏已经确认消失的格，保留同一对象仍存在的其余部分。
  removed(changes) {
    for (const change of changes) if (change.after !== change.before) this.#forget(cellKey(change));
  }

  // 最后一格移除时一并释放原对象在支撑索引里的记录。
  #forget(key) {
    const object = this.#cells.get(key);
    if (!object) return;
    this.#cells.delete(key);
    if (object.keys.some(other => this.#cells.get(other) === object)) return;
    for (const support of object.supports) {
      const owners = this.#supports.get(support);
      owners.delete(object);
      if (!owners.size) this.#supports.delete(support);
    }
  }
}

// 在当前世界内把整格坐标映射为稳定索引。
function cellKey({ x, y, z }) { return `${x},${y},${z}`; }

module.exports = { BuiltObjects };
