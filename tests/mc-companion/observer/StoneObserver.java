package dafeiyu.fixture;

import com.destroystokyo.paper.event.entity.EntityAddToWorldEvent;
import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import java.io.BufferedWriter;
import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardOpenOption;
import java.util.UUID;
import org.bukkit.Bukkit;
import org.bukkit.Location;
import org.bukkit.Material;
import org.bukkit.command.Command;
import org.bukkit.command.CommandSender;
import org.bukkit.entity.Entity;
import org.bukkit.entity.Item;
import org.bukkit.entity.Player;
import org.bukkit.event.EventHandler;
import org.bukkit.event.EventPriority;
import org.bukkit.event.Listener;
import org.bukkit.event.block.BlockDropItemEvent;
import org.bukkit.event.entity.EntityPickupItemEvent;
import org.bukkit.event.entity.ItemMergeEvent;
import org.bukkit.event.player.PlayerDropItemEvent;
import org.bukkit.plugin.java.JavaPlugin;

// 在独立测试世界旁路记录圆石来源，各处理器仅读取游戏对象。
public final class StoneObserver extends JavaPlugin implements Listener {
    private final String stream = UUID.randomUUID().toString();
    private long sequence;
    private BufferedWriter output;

    // 建立本次观察流并在文件可用后注册只读事件。
    // 步骤：创建专用目录 → 打开独立事件文件 → 登记监听。
    @Override public void onEnable() {
        try {
            var directory = Files.createDirectories(getDataFolder().toPath());
            output = Files.newBufferedWriter(directory.resolve(stream + ".jsonl"),
                StandardCharsets.UTF_8, StandardOpenOption.CREATE_NEW);
            Files.writeString(directory.resolve("active-stream"), stream, StandardCharsets.UTF_8);
        } catch (IOException error) {
            throw new UncheckedIOException(error);
        }
        getServer().getPluginManager().registerEvents(this, this);
    }

    // 关闭观察文件，使已记录的回执可在停服后离线读取。
    @Override public void onDisable() {
        if (output == null) return;
        try { output.close(); }
        catch (IOException error) { throw new UncheckedIOException(error); }
    }

    // 复制来源序号、服务端时刻与世界位置，保留不同世界间的身份边界。
    private JsonObject event(String kind, Location place) {
        var row = new JsonObject();
        row.addProperty("kind", kind);
        row.addProperty("stream", stream);
        row.addProperty("seq", ++sequence);
        row.addProperty("tick", Bukkit.getCurrentTick());
        row.addProperty("world", place.getWorld().getUID().toString());
        row.addProperty("x", place.getX());
        row.addProperty("y", place.getY());
        row.addProperty("z", place.getZ());
        return row;
    }

    // 复制物品实体的稳定身份和该连接中可对齐的网络编号。
    private void item(JsonObject row, Item entity) {
        row.addProperty("item", entity.getUniqueId().toString());
        row.addProperty("networkId", entity.getEntityId());
        row.addProperty("amount", entity.getItemStack().getAmount());
    }

    // 逐条保存证据，写失败时保留故障并让其后的序号暴露缺口。
    private void record(JsonObject row) {
        try {
            output.write(row.toString());
            output.newLine();
            output.flush();
        } catch (IOException error) {
            throw new UncheckedIOException(error);
        }
    }

    // 保存原方块、破坏者和候选掉落，允许整次破坏产生零件圆石。
    // 步骤：筛选实际方块 → 记录每个候选实体 → 记录本次破坏的候选总量。
    @EventHandler(priority = EventPriority.MONITOR, ignoreCancelled = true)
    public void blockDrop(BlockDropItemEvent drop) {
        if (drop.getBlockState().getType() != Material.COBBLESTONE) return;
        int total = 0;
        for (Item entity : drop.getItems()) {
            if (entity.getItemStack().getType() != Material.COBBLESTONE) continue;
            var row = event("blockDrop", drop.getBlock().getLocation());
            row.addProperty("actor", drop.getPlayer().getUniqueId().toString());
            item(row, entity);
            record(row);
            total += entity.getItemStack().getAmount();
        }

        var row = event("blockBroken", drop.getBlock().getLocation());
        row.addProperty("actor", drop.getPlayer().getUniqueId().toString());
        row.addProperty("candidateAmount", total);
        record(row);
    }

    // 记录玩家从库存中主动丢出的圆石身份。
    @EventHandler(priority = EventPriority.MONITOR, ignoreCancelled = true)
    public void playerDrop(PlayerDropItemEvent drop) {
        Item entity = drop.getItemDrop();
        if (entity.getItemStack().getType() != Material.COBBLESTONE) return;
        var row = event("playerDrop", entity.getLocation());
        row.addProperty("actor", drop.getPlayer().getUniqueId().toString());
        item(row, entity);
        record(row);
    }

    // 记录加入世界的实体和同时在场的其他圆石，独立于掉落原因回调。
    // 步骤：筛选物品 → 读取同一时点附近实体 → 写入观察。
    @EventHandler(priority = EventPriority.MONITOR)
    public void added(EntityAddToWorldEvent added) {
        Entity entity = added.getEntity();
        if (!(entity instanceof Item drop) || drop.getItemStack().getType() != Material.COBBLESTONE) return;
        var row = event("added", drop.getLocation());
        item(row, drop);
        var others = new JsonArray();
        for (var nearby : drop.getNearbyEntities(5, 5, 5)) {
            if (nearby instanceof Item other && other.getItemStack().getType() == Material.COBBLESTONE)
                others.add(other.getUniqueId().toString());
        }
        row.add("nearbyStone", others);
        record(row);
    }

    // 返回玩家存储栏中实际携带的圆石数量，装备格不参与本夹具。
    private int stock(Player player) {
        int count = 0;
        for (var stack : player.getInventory().getStorageContents()) {
            if (stack != null && stack.getType() == Material.COBBLESTONE) count += stack.getAmount();
        }
        return count;
    }

    // 仅接受本机夹具控制台的采样请求，同时读取角色、世界、位置和库存。
    // 步骤：核对控制台及请求标记 → 在同一服务器线程采样 → 记录独立回执。
    @Override public boolean onCommand(CommandSender sender, Command command, String label, String[] args) {
        if (sender != Bukkit.getConsoleSender() || args.length != 2 || !args[1].matches("[a-zA-Z0-9-]{1,48}")) return false;
        Player player = Bukkit.getPlayerExact(args[0]);
        if (player == null) return false;
        var row = event("sample", player.getLocation());
        row.addProperty("actor", player.getUniqueId().toString());
        row.addProperty("request", args[1]);
        row.addProperty("stock", stock(player));
        var stacks = new JsonArray();
        for (var stack : player.getInventory().getStorageContents()) {
            if (stack != null && stack.getType() == Material.COBBLESTONE) stacks.add(stack.getAmount());
        }
        row.add("stacks", stacks);
        record(row);
        return true;
    }

    // 保存合堆双方的事前数量，再从下一次服务器推进读回实际去向。
    // 步骤：绑定源和目标 → 保存候选转移 → 在下一 tick 读取双方的在场数量。
    @EventHandler(priority = EventPriority.MONITOR, ignoreCancelled = true)
    public void merged(ItemMergeEvent merge) {
        Item source = merge.getEntity();
        Item target = merge.getTarget();
        if (source.getItemStack().getType() != Material.COBBLESTONE) return;
        var row = event("merge", source.getLocation());
        item(row, source);
        row.addProperty("target", target.getUniqueId().toString());
        row.addProperty("targetAmount", target.getItemStack().getAmount());
        record(row);

        Bukkit.getScheduler().runTask(this, () -> {
            var after = event("mergeReadback", target.getLocation());
            after.addProperty("cause", row.get("seq").getAsLong());
            after.addProperty("sourceRemaining", source.isValid() ? source.getItemStack().getAmount() : 0);
            after.addProperty("targetRemaining", target.isValid() ? target.getItemStack().getAmount() : 0);
            record(after);
        });
    }

    // 保存拾取候选并用玩家实存与地面余量读回实际效果。
    // 步骤：核对角色与物品 → 保存拾取前数量 → 在下个 tick 读回，保留原候选关联。
    @EventHandler(priority = EventPriority.MONITOR, ignoreCancelled = true)
    public void pickup(EntityPickupItemEvent pickup) {
        Item entity = pickup.getItem();
        if (!(pickup.getEntity() instanceof Player player) || entity.getItemStack().getType() != Material.COBBLESTONE) return;
        var row = event("pickup", entity.getLocation());
        item(row, entity);
        row.addProperty("actor", player.getUniqueId().toString());
        row.addProperty("beforeStock", stock(player));
        row.addProperty("remaining", pickup.getRemaining());
        record(row);

        Bukkit.getScheduler().runTask(this, () -> {
            var after = event("pickupReadback", player.getLocation());
            after.addProperty("cause", row.get("seq").getAsLong());
            after.addProperty("actor", player.getUniqueId().toString());
            after.addProperty("afterStock", stock(player));
            after.addProperty("ground", entity.isValid() ? entity.getItemStack().getAmount() : 0);
            record(after);
        });
    }
}
