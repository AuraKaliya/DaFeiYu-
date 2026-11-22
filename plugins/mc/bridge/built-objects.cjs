// 将同一游戏与维度内的实际放置保留为可独立于任务使用的对象。
class BuiltObjects {
  #maps = new WeakMap();
  #cells = new Map();
  #supports = new Map();

  // 切换到本次动作的地图保护表，地图身份由游戏连接持有。
  // 步骤：查找原地图记录 → 首次建立索引 → 选定此次操作的方格与支撑。
  bind(binding) {
    if (!this.#maps.has(binding)) this.#maps.set(binding, { cells: new Map(), supports: new Map() });
    const { cells, supports } = this.#maps.get(binding);
    this.#cells = cells;
    this.#supports = supports;
  }

  // 保存一个经游戏读回的对象，各格共享同一组支撑关系。
  placed(cells) {
    const entries = new Map(cells.map(cell => [cellKey(cell), cell]));
    const supports = new Set(cells.map(cell => cellKey({ ...cell, y: cell.y - 1 }))
      .filter(key => !entries.has(key)));
    const object = { entries, supports };
    for (const key of entries.keys()) {
      this.#forget(key);
      this.#cells.set(key, object);
    }
    for (const key of supports) {
      if (!this.#supports.has(key)) this.#supports.set(key, new Set());
      this.#supports.get(key).add(object);
    }
  }

  // 对选定目标和伴随变更核对实际对象及支撑。
  // 步骤：找到涉及的对象 → 复核已加载的格并移除已改变的记录 → 核对拆改许可。
  permits(blocks, approved, read) {
    const keys = blocks.map(({ position }) => cellKey(position));
    const objects = new Set();
    for (const key of keys) {
      if (this.#cells.has(key)) objects.add(this.#cells.get(key));
      for (const object of this.#supports.get(key) ?? []) objects.add(object);
    }
    for (const object of objects) for (const [key, cell] of object.entries) {
      if (this.#cells.get(key) !== object) continue;
      const actual = read(cell);
      if (actual && (actual.name !== cell.block || (cell.half && actual.getProperties().half !== cell.half))) this.#forget(key);
    }

    const requested = this.#cells.get(keys[0]);
    return keys.every(key => {
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
    object.entries.delete(key);
    if (object.entries.size) return;
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
