const { goals } = require('mineflayer-pathfinder');
const { belowFeet } = require('./world.cjs');

// 读取候选脚位是否位于有完整承重面的干地。
function dryStand(bot, point) {
  const feet = point.floored();
  const cell = bot.blockAt(feet), below = bot.blockAt(feet.offset(0, -1, 0));
  return cell?.boundingBox === 'empty' && !['water', 'lava', 'bubble_column'].includes(cell.name) &&
    !cell.getProperties().waterlogged && below?.boundingBox === 'block';
}

// 检查精确站位的脚格、承重面及头部空间均已加载且可用。
function clearStand(bot, point) {
  const head = bot.blockAt(point.floored().offset(0, 1, 0));
  return dryStand(bot, point) && head?.boundingBox === 'empty' &&
    !['water', 'lava', 'bubble_column'].includes(head.name) && !head.getProperties().waterlogged;
}

// 把固定目的地的可接受站位限制到干地。
class DryStandGoal extends goals.Goal {
  // 保存寻路期间读取的客户端世界。
  constructor(bot) {
    super();
    this.bot = bot;
  }

  // 判定候选节点能否在干地结束工作。
  isEnd(node) { return dryStand(this.bot, node); }
}

// 从实际眼高检查目标中心及近顶面，允许站到格心稍外时仍命中同一个目标。
// 步骤：排除脚下支撑 → 分别检查距离和遮挡 → 确认视线命中的原格。
function digReachable(bot, target, feet, range = 4.5) {
  if (target.equals(belowFeet(feet))) return false;
  const eye = feet.offset(0, bot.entity.eyeHeight, 0);
  return [0.5, 0.875].some(height => {
    const direction = target.offset(0.5, height, 0.5).minus(eye);
    const distance = direction.norm();
    if (distance > range) return false;
    const hit = bot.world.raycast(eye, direction.normalize(), distance,
      (block, ray) => block.position.equals(target) || Boolean(ray.intersect(block.shapes, block.position)));
    return hit?.position.equals(target) ?? false;
  });
}

// 用同一可达条件核对候选脚位和实际到达，保留寻路器到目标方块的估价。
class DigStandGoal extends goals.GoalLookAtBlock {
  // 固定原目标，不在寻路过程中替换采掘格。
  constructor(bot, target) {
    super(target, bot.world);
    this.bot = bot;
  }

  // 寻路可能在踏入格边时结束，计划格心为实际脚位预留距离余量。
  isEnd(node) { return digReachable(this.bot, this.pos, this.pos.clone().set(node.x + 0.5, node.y, node.z + 0.5), 3.8); }
}

// 核对双方高差仍在可视为同一跟随站位的范围内。
function followHeightClose(vertical) {
  return Math.abs(vertical) < 2;
}

// 用同一个动态目标跨水，干岸玩家的到达条件也要求跟随者能落在干岸。
class SurfaceFollowGoal extends goals.GoalFollow {
  // 记录实际实体和寻路使用的客户端世界。
  constructor(bot, entity, range) {
    super(entity, range);
    this.bot = bot;
  }

  // 在原跟随距离条件上叠加高差和干岸站位约束。
  isEnd(node) {
    return super.isEnd(node) && followHeightClose(node.y - this.y) &&
      (!dryStand(this.bot, this.entity.position) || dryStand(this.bot, node));
  }
}

// 一次会合只追踪起步时绑定的配对角色，目标离开原移动范围即停止。
class PlayerMeetingGoal extends SurfaceFollowGoal {
  // 固定双方实体、配对记录、地图和本次移动的起点。
  constructor(bot, player) {
    super(bot, player.entity, 1);
    this.player = player;
    this.actor = bot.entity;
    this.origin = this.actor.position.clone();
    this.dimension = bot.game.dimension;
  }

  // 目标丢失、同编号实体被替换和原范围失效时分别阻止继续移动。
  issue() {
    const point = this.entity.position;
    if (this.bot.entity !== this.actor || this.bot.game.dimension !== this.dimension ||
        this.player.entity !== this.entity || this.player.uuid !== this.entity.uuid || this.entity.isValid === false ||
        this.bot.entities[this.entity.id] !== this.entity || !point || ![point.x,point.y,point.z].every(Number.isFinite))
      return 'meeting_player_unavailable';
    return this.origin.distanceTo(point) > 64 ? 'meeting_out_of_range' : '';
  }

  // 寻路器在每次更新路径之前复核目标身份和原活动范围。
  isValid() { return !this.issue(); }

  // 玩家进入另一格时重算同一工作目标，当前位置变更由寻路器消费。
  hasChanged() {
    const next = this.entity.position.floored();
    if (this.x === next.x && this.y === next.y && this.z === next.z) return false;
    this.x = next.x;
    this.y = next.y;
    this.z = next.z;
    return true;
  }
}

// 在真实物理帧跟踪入水、游动、离水与落地，终态也保存实际角色位置。
// 步骤：登记首个样本 → 跟踪水面和干地转换 → 在工作退出时收回采样。
function monitorSurface(bot, result) {
  const navigation = result.navigation = { crossings: 0, phase: 'land' };
  let wasWet = false;
  const sample = () => {
    const actor = bot.entity;
    if (!actor?.position) return navigation.phase;
    const feet = actor.position.floored();
    const wet = actor.isInWater || [feet,feet.offset(0,-1,0)].some(point => bot.blockAt(point)?.name === 'water');
    const dry = !wet && actor.onGround && dryStand(bot, actor.position);
    if (dry && wasWet) {
      ++navigation.crossings;
      wasWet = false;
    }
    if (wet) wasWet = true;
    navigation.phase = wet ? 'swimming' : wasWet ? 'leaving_water' : navigation.crossings ? 'dry' : 'land';
    return navigation.phase;
  };
  sample();
  bot.on('physicsTick', sample);
  return { sample, stop() { sample(); bot.removeListener('physicsTick', sample); } };
}

module.exports = { dryStand, clearStand, DryStandGoal, digReachable, DigStandGoal, followHeightClose, SurfaceFollowGoal, PlayerMeetingGoal, monitorSurface };
