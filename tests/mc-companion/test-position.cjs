class Position {
  // 保存本次测试的世界坐标。
  constructor(x, y, z) { this.set(x, y, z); }
  // 替换当前值并返回供原话视线使用的位置。
  set(x, y, z) { Object.assign(this, { x, y, z }); return this; }
  // 复制当前值，模拟游戏取样脱离实时实体。
  clone() { return this.offset(0, 0, 0); }
  // 提供格子附近相对坐标。
  offset(x, y, z) { return new Position(this.x + x, this.y + y, this.z + z); }
  // 将实体位置落在方块坐标上。
  floored() { return new Position(...[this.x, this.y, this.z].map(Math.floor)); }
  // 核对两个站位格是否一致。
  equals(p) { return this.x === p.x && this.y === p.y && this.z === p.z; }
  // 返回两个实际采样中心的距离。
  distanceTo(p) { return Math.hypot(this.x - p.x, this.y - p.y, this.z - p.z); }
}

module.exports = { Position };
