// 为一个游戏角色维护唯一控制权，读回原工作与是否仍持权相互独立。
// 步骤：接纳或续期 → 显式退出或超时后停止原工作 → 实际退出后交还角色。
function controllerLease(drain, now = () => performance.now()) {
  let holder, release, expires = 0;
  const retired = new Set();

  // 封闭接纳并等待原异步操作实际结束，失败时保留未确认状态。
  // 步骤：复用原释放 → 等待执行层结算 → 按结果交还控制或保留占用。
  function stop() {
    if (release) return release.phase;
    const receipt = { phase: 'requested' };
    release = receipt;
    Promise.resolve().then(drain).then(() => {
      receipt.phase = 'released';
      holder = release = undefined;
    }, () => { receipt.phase = 'unconfirmed'; });
    return receipt.phase;
  }

  // 无持续收件续期的控制实例在交接前进入原动作退出流程。
  function expire() { if (holder && now() >= expires) stop(); }

  return {
    // 为首次监听登记持权者，后续只允许原实例在有效期内续期。
    // 步骤：检查原持权期限 → 排除已退出和竞争实例 → 更新本次监听期限。
    claim(controller) {
      expire();
      if (release || retired.has(controller) || (holder && holder !== controller)) return false;
      holder = controller;
      expires = now() + 8000;
      return true;
    },
    // 只能关闭原实例的控制，旧实例重投只读取释放状态。
    detach(controller) {
      if (holder !== controller) return 'released';
      retired.add(controller);
      return stop();
    },
    // 核对执行器的新工作是否仍来自当前有效持权者。
    owns(controller) { expire(); return !release && holder === controller; },
  };
}

module.exports = { controllerLease };
