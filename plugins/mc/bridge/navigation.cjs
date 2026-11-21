const { goals } = require('mineflayer-pathfinder');

// 读取候选脚位是否位于有完整承重面的干地。
function dryStand(bot, point) {
  const feet = point.floored();
  const cell = bot.blockAt(feet), below = bot.blockAt(feet.offset(0, -1, 0));
  return cell?.boundingBox === 'empty' && !['water', 'lava', 'bubble_column'].includes(cell.name) &&
    !cell.getProperties().waterlogged && below?.boundingBox === 'block';
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

// 用同一个动态目标跨水，干岸玩家的到达条件也要求跟随者能落在干岸。
class SurfaceFollowGoal extends goals.GoalFollow {
  // 记录实际实体和寻路使用的客户端世界。
  constructor(bot, entity, range) {
    super(entity, range);
    this.bot = bot;
  }

  // 在原跟随距离条件上叠加干岸站位约束。
  isEnd(node) {
    return super.isEnd(node) && (!dryStand(this.bot, this.entity.position) || dryStand(this.bot, node));
  }
}

// 在真实物理帧跟踪入水、游动、离水与落地，终态也保存实际角色位置。
// 步骤：登记首个样本 → 跟踪水面和干地转换 → 在工作退出时收回采样。
function monitorSurface(bot, result) {
  const navigation = result.navigation = { crossings: 0, phase: 'land' };
  let wasWet = false;
  const sample = () => {
    const actor = bot.entity;
    if (!actor) return navigation.phase;
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

module.exports = { dryStand, DryStandGoal, SurfaceFollowGoal, monitorSurface };
