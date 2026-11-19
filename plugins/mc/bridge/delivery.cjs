// 在当前游戏世界保存宿主签发的新委托，接单本身不启动采掘。
function collectionDesk(world, inbox, currentQuestion, accepting = () => true) {
  let accepted = null;
  // 查询当前世界能否明确证明某项委托已接收或尚未接收。
  function read(generation, question, id) {
    const current = currentQuestion();
    if (!current?.sent || current.generation !== generation || current.id !== question ||
      world.read()?.generation !== generation) return null;
    if (accepted?.generation === generation && accepted.question === question) {
      if (accepted.grant !== id) throw new Error('同一问题已接纳其他委托');
      return { ...accepted };
    }
    return { generation, question, grant: id, phase: 'unreceived' };
  }
  return {
    read,
    // 核对签发范围和原玩家答复，重复命令只能重取同一接单事实。
    // 步骤：验证原问题与范围 → 复用或拒绝已有接单 → 对游戏入口复核消息 → 保存新回执。
    admit(args) {
      const { generation, question, grant, owner, order, quantity, material, returnPlace, via, command } = args;
      if (![question, grant].every(id => typeof id === 'string' && /^[1-9][0-9]{0,19}$/.test(id)) ||
        !Number.isSafeInteger(quantity) || quantity < 1 || quantity > 64 || !['game', 'management'].includes(via) ||
        typeof command !== 'string' || !/^[1-9][0-9]{0,19}$/.test(command)) throw new Error('无效采集委托');
      const original = currentQuestion();
      const receipt = read(generation, question, grant);
      if (!receipt || original.owner !== owner || original.order !== order || material !== original.material ||
        !original.returnPlace || returnPlace !== original.returnPlace) throw new Error('采集范围或世界已变');
      const value = { generation, question, grant, owner, order, quantity, material, returnPlace, via, command, phase: 'accepted' };
      if (receipt.phase === 'accepted') {
        for (const key of Object.keys(value)) if (value[key] !== receipt[key]) throw new Error('委托参数冲突');
        return receipt;
      }
      if (!accepting()) throw new Error('角色已停止接纳委托');
      if (via === 'game') {
        const sequence = Number(command);
        const response = inbox.get(generation, sequence);
        if (!Number.isSafeInteger(sequence) || sequence <= original.after || response?.offer?.purpose !== 'collect.return' ||
          response.offer.question !== question || response.offer.quantity !== quantity) throw new Error('找不到原玩家采集授权');
      }
      accepted = value;
      return { ...accepted };
    },
  };
}
module.exports = { collectionDesk };
