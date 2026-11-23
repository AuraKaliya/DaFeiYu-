// 统一原版物品标识与游戏注册表使用的短名。
function itemName(name) { return typeof name === 'string' ? name.replace(/^minecraft:/, '') : ''; }

// 读取实际可用于合成的背包格及堆叠数量，排除装备和合成窗口槽位。
function slots(bot) { return bot.inventory.slots.slice(bot.inventory.inventoryStart, bot.inventory.inventoryEnd); }

// 匹配配方物品及指定变种，空元数据表示不限变种。
function matches(stack, entry) { return stack?.type === entry.id && (entry.metadata == null || stack.metadata === entry.metadata); }

// 按配方物品汇总背包中的实际数量。
function count(stacks, entry) { return stacks.reduce((n, stack) => matches(stack, entry) ? n + stack.count : n, 0); }

// 搜索当前站位可用的工作台，复核时只读取原选中的位置。
// 步骤：读取候选格 → 核对实际方块、距离与视线。
function craftingTable(bot, position) {
  const id = bot.registry.blocksByName.crafting_table?.id;
  if (id === undefined) return null;
  const points = position ? [position] : bot.findBlocks({ matching: id, maxDistance: 4, count: 16 });
  return points.map(point => bot.blockAt(point)).find(block => block?.type === id &&
    block.position.distanceTo(bot.entity.position) <= 4 && bot.canSeeBlock(block)) ?? null;
}

// 检查每批成品都能入包，计入到该批为止已由原料腾出的槽位。
// 步骤：统计已有容量与原料堆叠 → 按批核对腾空后的容量。
function fits(stacks, { input, output, stackSize }, batches) {
  const room = stacks.reduce((n, stack) => n + (!stack ? stackSize :
    matches(stack, output) && !stack.nbt ? stackSize - stack.count : 0), 0);
  let stock = 0;
  const freedAt = stacks.filter(stack => matches(stack, input)).map(stack => (stock += stack.count));
  for (let batch = 1; batch <= batches; ++batch) {
    const freed = freedAt.filter(threshold => threshold <= -input.count * batch).length;
    if (room + freed * stackSize < output.count * batch) return false;
  }
  return true;
}

// 给游戏动作保留可读的合成受阻原因。
function blocked(code, reason) { return Object.assign(new Error(reason), { code }); }

// 从当前游戏配方表选择原木到木板的可执行配方。
// 步骤：筛出单一原料和产物 → 核对足量原料、工作台与容量 → 优先使用背包格。
function chooseRecipe(bot, args, result) {
  const product = bot.registry.itemsByName[result.item];
  if (!product || !result.item.endsWith('_planks') || !Number.isSafeInteger(args.count) || args.count < 1 || args.count > 32)
    throw blocked('unsupported_craft_item', '本技能需要当前游戏中的木板和 1～32 的目标产量');

  const stacks = slots(bot);
  const choices = [];
  for (const recipe of bot.recipesAll(product.id, null, true)) {
    const [input] = recipe.delta.filter(entry => entry.count < 0);
    const output = recipe.result;
    const ingredient = bot.registry.items[input?.id]?.name;
    if (!ingredient || !/_(log|stem)$/.test(ingredient) || (args.ingredient && itemName(args.ingredient) !== ingredient) ||
        recipe.delta.length !== 2 || !Number.isSafeInteger(output.count) || output.count < 1 ||
        !recipe.delta.some(entry => entry.id === output.id && entry.count === output.count)) continue;
    const batches = Math.ceil(args.count / output.count);
    choices.push({ recipe, input, output, ingredient, batches, stackSize: product.stackSize });
  }
  if (!choices.length) throw blocked('recipe_unavailable', '当前配方表没有匹配的原木合成方式');
  const stocked = choices.filter(choice => count(stacks, choice.input) >= -choice.input.count * choice.batches);
  if (!stocked.length) throw blocked('insufficient_materials', '当前背包里的原木不足以完成所需批次');
  const table = stocked.some(choice => choice.recipe.requiresTable) ? craftingTable(bot) : null;
  const reachable = stocked.filter(choice => !choice.recipe.requiresTable || table);
  if (!reachable.length) throw blocked('crafting_table_unavailable', '当前站位没有可用工作台');
  const ready = reachable.filter(choice => fits(stacks, choice, choice.batches));
  const choice = ready.find(candidate => !candidate.recipe.requiresTable) ?? ready[0];
  if (!choice) throw blocked('inventory_full', '背包没有足够的木板容量');
  return { ...choice, table: choice.recipe.requiresTable ? table.position : null };
}

// 核对服务端背包，合成点击产生的中间全量包可能先于最终入包到达。
// 步骤：请求全量读回 → 核对当前批次增减 → 仅在读回不符时跨游戏刻再查。
async function readBatch(bot, { input, output }, materialBefore, outputBefore) {
  for (let read = 1; ; ++read) {
    // 锁定版本的同步只等待下一个全量包，可能读到原料仍在合成格的状态。
    await bot._syncWindow(bot.inventory);
    const after = slots(bot);
    const consumed = materialBefore - count(after, input);
    const produced = count(after, output) - outputBefore;
    if ((consumed === -input.count && produced === output.count) || read === 3) return { consumed, produced };
    await bot.waitForTicks(1);
  }
}

// 逐批执行选中的配方，同一工作保留已发生的耗料与产出。
// 步骤：查询并选择配方 → 每批复核原条件 → 读回实际背包变化并核对产量。
async function craftPlanks(bot, args, check, result) {
  const progress = { consumed: 0, produced: 0, remaining: args.count };
  Object.assign(result, { item: itemName(args.item), count: args.count, craft: progress, craftAttempted: false, inventoryDelta: {} });
  const choice = chooseRecipe(bot, args, result);
  const { input, output, ingredient, batches, recipe, table } = choice;
  Object.assign(progress, { ingredient, perBatch: output.count, requiresTable: Boolean(table), batches, completedBatches: 0 });

  for (let batch = 0; batch < batches; ++batch) {
    check();
    const station = table ? craftingTable(bot, table) : null;
    const before = slots(bot);
    const materialBefore = count(before, input), outputBefore = count(before, output);
    const remainingBatches = batches - batch;
    if (bot.currentWindow || (table && !station) || materialBefore < -input.count * remainingBatches || !fits(before, choice, remainingBatches))
      throw blocked('craft_conditions_changed', '当前窗口、工作台、原料或背包容量无法完成剩余批次');

    result.craftAttempted = true;
    let consumed, produced;
    try { await bot.craft(recipe, 1, station); }
    finally {
      try { ({ consumed, produced } = await readBatch(bot, choice, materialBefore, outputBefore)); }
      catch { throw blocked('craft_inventory_unconfirmed', '本批提交后的服务端库存未确认'); }
      for (const [name, change] of [[ingredient, -consumed], [result.item, produced]]) {
        const total = (result.inventoryDelta[name] ?? 0) + change;
        if (total) result.inventoryDelta[name] = total;
        else delete result.inventoryDelta[name];
      }
      progress.consumed = Math.max(0, -(result.inventoryDelta[ingredient] ?? 0));
      progress.produced = Math.max(0, result.inventoryDelta[result.item] ?? 0);
      progress.remaining = Math.max(0, args.count - progress.produced);
    }
    if (consumed !== -input.count || produced !== output.count)
      throw blocked('craft_unverified', '实际耗料或木板入包量与配方不符');
    ++progress.completedBatches;
  }
}

module.exports = { craftPlanks };
