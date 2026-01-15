import socket
import threading
import sys
import time
from collections import defaultdict
import math
from datetime import datetime

# ===================== 全局配置（核心修改：帧率提升到20帧/秒）=====================
client_sockets = []
client_id_map = {}  # socket → player_id
client_lock = threading.Lock()  # 保护客户端映射的线程安全
next_player_id = 1
CHECK_DEAD_CONN_INTERVAL = 5  # 死连接检测间隔
MAX_MSG_PER_TICK = 10  # 限制单帧消息数
MAX_MSG_PER_SECOND = 100  # 与客户端发送频率匹配
SEND_BUFFER_SIZE = 4096  # 缓冲区大小
GAME_TICK_INTERVAL = 0.05  # 核心修改：从0.1→0.05秒（1/0.05=20帧/秒）
MOVE_SPEED = 2.5  # 核心修改：从5.0→2.5（帧率翻倍，速度减半保证总移动速度不变）
ROTATE_SPEED = 3.0  # 核心修改：从6.0→3.0（帧率翻倍，转向速度减半保证总转向速度不变）
MAP_BOUND_X = (100.0, 2000.0)  # 地图边界
MAP_BOUND_Y = (100.0, 2000.0)
game_running = True

# 开火/碰撞配置（按需求调整）
FIRE_RAY_LENGTH = 1000.0  # 开火射线长度（单位：游戏单位）
PLAYER_COLLISION_RADIUS = 50.0  # 玩家碰撞半径（100×100×100立方体→球体半径50）
FIRE_DAMAGE = 2  # 每次命中扣除HP（每帧2点）
SCORE_PER_HIT = 1  # 每次命中增加的得分

# 协议相关新增配置
SCORE_BROADCAST_INTERVAL = 5.0  # 得分协议广播间隔（5秒）

# 玩家状态（含动画状态）
player_states = defaultdict(dict)
player_key_states = defaultdict(lambda: {"W": False, "S": False, "A": False, "D": False})
player_rotate_states = defaultdict(lambda: "s")  # "l"左 "r"右 "s"停止
player_scores = defaultdict(int)  # 新增：玩家得分字典（pid → 得分）
player_death_flag = defaultdict(bool)  # 新增：玩家死亡标记（避免重复发送死亡协议）

# 开火锁定状态（pid → {是否锁定、定格位置x/y、定格转向yaw}）
fire_lock_states = defaultdict(dict)
# 命中状态记录（pid → 是否被命中，用于播放受伤动画）
hit_players = defaultdict(bool)
# 命中结果记录（用于判断是否真的命中）
fire_hit_results = defaultdict(bool)

# 线程安全锁（新增score_lock保护得分字典）
state_lock = threading.Lock()
command_stats = defaultdict(int)
stats_lock = threading.Lock()
fire_lock = threading.Lock()  # 保护开火/命中状态
score_lock = threading.Lock()  # 新增：保护得分字典的线程锁
last_stats_print_time = time.time()

# 协议映射（k|f=开火按住，k|nf=开火松开）
KEY_PROTOCOL_MAP = {
    # 移动按键
    "1": ("W", True),  # W按下
    "2": ("S", True),  # S按下
    "3": ("A", True),  # A按下
    "4": ("D", True),  # D按下
    "m": ("W", False),  # W松开
    "n": ("S", False),  # S松开
    "p": ("A", False),  # A松开
    "q": ("D", False),  # D松开
    # 开火按键
    "f": ("FIRE", True),  # 开火按住（鼠标左键按下）
    "nf": ("FIRE", False)  # 开火松开（鼠标左键松开）
}

# 玩家默认状态（ani_id：0=Idle 1=Move 2=开火 3=受伤）
DEFAULT_PLAYER_STATE = {
    "x": 500.0, "y": 600.0, "z": 90.0,
    "roll": 0.0, "pitch": 0.0, "yaw": 90.0, "hp": 100,
    "last_x": 500.0, "last_y": 600.0,
    "ani_id": 0
}
MOVE_THRESHOLD = 0.1  # 移动判定阈值


# ===================== 工具函数（新增得分/死亡协议相关）=====================
def log(msg):
    """普通日志"""
    now = datetime.now().strftime("[%H:%M:%S]")
    print(f"{now} 📢 {msg}")


def log_error(msg):
    """错误日志"""
    now = datetime.now().strftime("[%H:%M:%S]")
    print(f"{now} ❌ {msg}")


def log_hit(msg):
    """命中日志（绿色字体）"""
    now = datetime.now().strftime("[%H:%M:%S]")
    # ANSI转义码：32=绿色，0=重置颜色
    print(f"{now} 🎯 \033[32m{msg}\033[0m")


def print_command_and_state_stats():
    """打印统计信息，包含开火/命中/得分状态"""
    global last_stats_print_time
    while game_running:
        current_time = time.time()
        if current_time - last_stats_print_time >= 1.0:
            with stats_lock:
                with state_lock:
                    with fire_lock:
                        with score_lock:
                            stats_msg = "📊 服务器状态汇总 → "
                            stats_parts = []
                            for pid in player_states.keys():
                                cmd_count = command_stats.get(pid, 0)
                                state = player_states.get(pid, DEFAULT_PLAYER_STATE)
                                pos_x = state["x"]
                                pos_y = state["y"]
                                hp = state["hp"]
                                score = player_scores.get(pid, 0)
                                ani_id = state["ani_id"]
                                ani_state = {0: "Idle", 1: "Move", 2: "Fire", 3: "Hit"}.get(ani_id, "Unknown")
                                fire_locked = fire_lock_states.get(pid, {}).get("is_locked", False)
                                stats_parts.append(
                                    f"玩家{pid}：命令{cmd_count}次 | 位置({pos_x:.1f},{pos_y:.1f}) | HP{hp} | 得分{score} | 动画{ani_state} | 开火锁定={fire_locked}"
                                )
                            stats_msg += " | ".join(stats_parts) if stats_parts else "暂无在线玩家"
                            log(stats_msg)
                            command_stats.clear()
                last_stats_print_time = current_time
        time.sleep(0.1)


def init_player(pid):
    """初始化玩家状态（新增得分/死亡标记初始化）"""
    try:
        with state_lock:
            player_states[pid] = DEFAULT_PLAYER_STATE.copy()
            player_key_states[pid] = {"W": False, "S": False, "A": False, "D": False}
            player_rotate_states[pid] = "s"
        with fire_lock:
            fire_lock_states.pop(pid, None)
            hit_players.pop(pid, None)
            fire_hit_results.pop(pid, None)
        with score_lock:
            player_scores[pid] = 0  # 初始化得分为0
        player_death_flag[pid] = False  # 初始化死亡标记为False
        log(f"玩家{pid}状态初始化完成（含得分/死亡状态，初始得分：0）")
    except Exception as e:
        log_error(f"初始化玩家{pid}状态失败：{str(e)}")


def calculate_forward(yaw_deg):
    """计算前向单位向量（仅X/Y平面，与客户端一致）"""
    try:
        # 将角度转换为弧度（UE中yaw是绕Z轴旋转，0度=X轴正方向）
        yaw_rad = math.radians(yaw_deg)
        forward_x = math.cos(yaw_rad)
        forward_y = math.sin(yaw_rad)
        # 归一化确保是单位向量
        magnitude = math.hypot(forward_x, forward_y)
        if magnitude > 0:
            forward_x /= magnitude
            forward_y /= magnitude
        return forward_x, forward_y
    except Exception as e:
        log_error(f"计算前向向量失败：{str(e)}")
        return 0.0, 0.0


def ray_sphere_intersection(ray_origin_x, ray_origin_y, ray_dir_x, ray_dir_y,
                            sphere_center_x, sphere_center_y, sphere_radius):
    """
    射线与球体碰撞检测（2D，X/Y平面）
    返回：是否碰撞、碰撞点距离射线起点的长度
    """
    # 计算射线起点到球体中心的向量
    s_to_c_x = sphere_center_x - ray_origin_x
    s_to_c_y = sphere_center_y - ray_origin_y

    # 计算该向量在射线方向上的投影长度
    tca = s_to_c_x * ray_dir_x + s_to_c_y * ray_dir_y
    # 投影长度为负 → 球体在射线反方向，无碰撞
    if tca < 0:
        return False, 0.0

    # 计算射线到球体中心的最短距离的平方
    d2 = (s_to_c_x * s_to_c_x + s_to_c_y * s_to_c_y) - tca * tca
    # 最短距离大于球体半径 → 无碰撞
    if d2 > sphere_radius * sphere_radius:
        return False, 0.0

    # 计算射线进入球体的点到投影点的距离
    thc = math.sqrt(sphere_radius * sphere_radius - d2)
    # 计算两个交点的距离（取最近的）
    t0 = tca - thc
    t1 = tca + thc

    # 取有效且最近的交点
    t = t0 if t0 > 0 else t1
    # 交点超出射线长度 → 无碰撞
    if t > FIRE_RAY_LENGTH:
        return False, 0.0

    return True, t


def broadcast_death_protocol(pid):
    """新增：广播死亡协议（d|id）给所有客户端"""
    if player_death_flag.get(pid, False):
        log(f"玩家{pid}已发送过死亡协议，跳过重复发送")
        return

    death_msg = f"d|{pid}"
    dead_sockets = []

    with client_lock:
        for sock in list(client_sockets):
            if not safe_send(sock, death_msg):
                dead_sockets.append(sock)

    # 清理发送失败的死连接
    if dead_sockets:
        with client_lock:
            for sock in dead_sockets:
                if sock in client_sockets:
                    client_sockets.remove(sock)
                client_id_map.pop(sock, None)
                try:
                    sock.close()
                except:
                    pass
        log(f"发送死亡协议时清理{len(dead_sockets)}个失效连接")

    player_death_flag[pid] = True
    log(f"📤 广播死亡协议：{death_msg}（玩家{pid}死亡/掉线）")


def build_score_msg():
    """新增：构建得分协议消息（s|playernum|id1|得分|id2|得分...）"""
    try:
        with client_lock:
            online_pids = list(client_id_map.values())
        with score_lock:
            msg_parts = ["s", str(len(online_pids))]
            for pid in online_pids:
                score = player_scores.get(pid, 0)
                msg_parts.extend([str(pid), str(score)])

        score_msg = "|".join(msg_parts)
        log(f"构建得分协议消息：{score_msg}")
        return score_msg
    except Exception as e:
        log_error(f"构建得分协议消息失败：{str(e)}")
        return "s|0"


def send_score_protocol_loop():
    """新增：得分协议广播线程（每5秒发送一次）"""
    log(f"得分协议广播线程启动 → 间隔{SCORE_BROADCAST_INTERVAL}秒")
    while game_running:
        time.sleep(SCORE_BROADCAST_INTERVAL)

        if not game_running:
            break

        score_msg = build_score_msg()
        dead_sockets = []

        with client_lock:
            for sock in list(client_sockets):
                if not safe_send(sock, score_msg):
                    dead_sockets.append(sock)

        # 清理发送失败的死连接
        if dead_sockets:
            with client_lock:
                for sock in dead_sockets:
                    if sock in client_sockets:
                        client_sockets.remove(sock)
                    client_id_map.pop(sock, None)
                    try:
                        sock.close()
                    except:
                        pass
            log(f"发送得分协议时清理{len(dead_sockets)}个失效连接")


def check_fire_hit(fire_pid):
    """
    重构命中检测：基于前向向量射线+球体碰撞（新增命中加分、HP归零发送死亡协议）
    返回：是否命中
    """
    try:
        with state_lock:
            # 1. 校验开火玩家状态
            if fire_pid not in player_states:
                log_error(f"开火玩家{fire_pid}状态不存在，跳过命中检测")
                return False
            fire_state = player_states[fire_pid]
            # 射线起点：开火玩家中心
            ray_origin_x = fire_state["x"]
            ray_origin_y = fire_state["y"]
            # 射线方向：开火玩家前向单位向量
            ray_dir_x, ray_dir_y = calculate_forward(fire_state["yaw"])

        hit_targets = []
        has_hit = False

        with state_lock:
            # 2. 遍历所有玩家检测射线碰撞
            for pid in player_states.keys():
                if pid == fire_pid:  # 跳过自己
                    continue
                target_state = player_states[pid]
                # 球体中心：目标玩家中心
                sphere_center_x = target_state["x"]
                sphere_center_y = target_state["y"]

                # 3. 执行射线-球体碰撞检测
                is_hit, hit_distance = ray_sphere_intersection(
                    ray_origin_x, ray_origin_y,
                    ray_dir_x, ray_dir_y,
                    sphere_center_x, sphere_center_y,
                    PLAYER_COLLISION_RADIUS
                )

                # 4. 判定有效命中（碰撞且在射线长度内）
                if is_hit and hit_distance > 0 and hit_distance <= FIRE_RAY_LENGTH:
                    hit_targets.append((pid, hit_distance))

        # 5. 处理命中结果（取最近的目标，避免穿透）
        if hit_targets:
            # 按碰撞距离排序，取最近的目标
            hit_targets.sort(key=lambda x: x[1])
            closest_pid, closest_distance = hit_targets[0]

            with fire_lock:
                with state_lock:
                    # 扣血（最低0）
                    old_hp = player_states[closest_pid]["hp"]
                    player_states[closest_pid]["hp"] = max(0, old_hp - FIRE_DAMAGE)
                    new_hp = player_states[closest_pid]["hp"]

                    # 标记为受伤（播放ani=3）
                    hit_players[closest_pid] = True

                    # 新增：命中玩家加分
                    with score_lock:
                        player_scores[fire_pid] += SCORE_PER_HIT

                    # 绿色打印命中日志（新增得分信息）
                    log_hit(
                        f"玩家{fire_pid}命中玩家{closest_pid}！碰撞距离：{closest_distance:.1f}单位，扣除{FIRE_DAMAGE}HP，剩余HP：{new_hp} | 玩家{fire_pid}得分+{SCORE_PER_HIT}（当前：{player_scores[fire_pid]}）")

                    # 新增：判断目标玩家HP是否归零，若是则发送死亡协议
                    if new_hp <= 0 and not player_death_flag[closest_pid]:
                        broadcast_death_protocol(closest_pid)

            has_hit = True
        else:
            log(f"玩家{fire_pid}开火未命中任何目标（射线长度：{FIRE_RAY_LENGTH}单位，碰撞半径：{PLAYER_COLLISION_RADIUS}单位）")

        return has_hit
    except Exception as e:
        log_error(f"命中检测失败：{str(e)}")
        return False


# ===================== 状态更新函数（无核心修改）=====================
def update_player_movement(pid):
    """更新玩家移动（开火按住时定格，松开后恢复；受伤不影响移动）"""
    try:
        with fire_lock:
            # 1. 检测是否处于开火锁定状态
            if pid in fire_lock_states and fire_lock_states[pid]["is_locked"]:
                # 锁定状态：强制恢复到定格位置，不更新移动
                with state_lock:
                    player_states[pid]["x"] = fire_lock_states[pid]["lock_x"]
                    player_states[pid]["y"] = fire_lock_states[pid]["lock_y"]
                    # 开火动画保持ani=2
                    player_states[pid]["ani_id"] = 2
                return

        # 2. 非锁定状态：正常更新移动
        with state_lock:
            if pid not in player_states or pid not in player_key_states:
                log_error(f"更新移动：玩家{pid}状态不存在，跳过")
                return
            state = player_states[pid]
            keys = player_key_states[pid]

            # 计算移动方向
            forward_x, forward_y = calculate_forward(state["yaw"])
            right_x = -forward_y
            right_y = forward_x
            dx, dy = 0.0, 0.0

            if keys["W"]:
                dx += forward_x * MOVE_SPEED
                dy += forward_y * MOVE_SPEED
            if keys["S"]:
                dx -= forward_x * MOVE_SPEED
                dy -= forward_y * MOVE_SPEED
            if keys["A"]:
                dx -= right_x * MOVE_SPEED
                dy -= right_y * MOVE_SPEED
            if keys["D"]:
                dx += right_x * MOVE_SPEED
                dy += right_y * MOVE_SPEED

            # 更新位置（地图边界限制）
            state["x"] = max(MAP_BOUND_X[0], min(state["x"] + dx, MAP_BOUND_X[1]))
            state["y"] = max(MAP_BOUND_Y[0], min(state["y"] + dy, MAP_BOUND_Y[1]))

            # 判断移动状态，设置动画（优先级：受伤(3) > 移动(1) > 静止(0)）
            current_x = state["x"]
            current_y = state["y"]
            last_x = state["last_x"]
            last_y = state["last_y"]
            move_distance = math.hypot(current_x - last_x, current_y - last_y)

            with fire_lock:
                if hit_players.get(pid, False):
                    # 受伤动画：仅保持1帧，之后恢复正常
                    state["ani_id"] = 3
                    hit_players[pid] = False
                else:
                    state["ani_id"] = 1 if move_distance > MOVE_THRESHOLD else 0

            state["last_x"] = current_x
            state["last_y"] = current_y
    except Exception as e:
        log_error(f"更新玩家{pid}移动失败：{str(e)}")


def update_player_rotation(pid):
    """更新玩家转向（开火按住时定格，松开后恢复）"""
    try:
        with fire_lock:
            # 1. 检测是否处于开火锁定状态
            if pid in fire_lock_states and fire_lock_states[pid]["is_locked"]:
                # 锁定状态：强制恢复到定格转向
                with state_lock:
                    player_states[pid]["yaw"] = fire_lock_states[pid]["lock_yaw"]
                return

        # 2. 非锁定状态：正常更新转向
        with state_lock:
            if pid not in player_states or pid not in player_rotate_states:
                log_error(f"更新转向：玩家{pid}状态不存在，跳过")
                return
            state = player_states[pid]
            rotate_state = player_rotate_states[pid]

            if rotate_state == "l":
                state["yaw"] -= ROTATE_SPEED
            elif rotate_state == "r":
                state["yaw"] += ROTATE_SPEED
            state["yaw"] = state["yaw"] % 360
    except Exception as e:
        log_error(f"更新玩家{pid}转向失败：{str(e)}")


# ===================== 协议解析（无核心修改）=====================
def parse_client_protocol(pid, msg, client_sock):
    """解析客户端协议：处理k|f（开火按住）、k|nf（开火松开）"""
    try:
        msg = msg.strip()
        if not msg or len(msg) < 2:
            log_error(f"玩家{pid}发送空消息，忽略")
            return

        # 限制每秒消息数
        with stats_lock:
            if command_stats[pid] >= MAX_MSG_PER_SECOND:
                log_error(f"玩家{pid}消息频率超限，忽略消息：{msg}")
                return
            command_stats[pid] += 1

        # 处理按键协议（k|key_code）
        if msg.startswith("k|"):
            parts = msg.split("|", 2)
            if len(parts) < 2 or parts[1].strip() == "":
                log_error(f"玩家{pid}按键协议格式错误：{msg}")
                return
            key_code = parts[1].strip()
            if key_code not in KEY_PROTOCOL_MAP:
                log_error(f"玩家{pid}未知按键码：{key_code}（支持：{list(KEY_PROTOCOL_MAP.keys())}）")
                return

            # 处理开火按住（k|f）
            if key_code == "f":
                with state_lock:
                    if pid not in player_states:
                        log_error(f"玩家{pid}状态不存在，无法开火")
                        return
                    # 1. 定格当前位置和转向
                    fire_state = player_states[pid]
                    lock_x = fire_state["x"]
                    lock_y = fire_state["y"]
                    lock_yaw = fire_state["yaw"]
                with fire_lock:
                    # 2. 标记为开火锁定状态
                    fire_lock_states[pid] = {
                        "is_locked": True,
                        "lock_x": lock_x,
                        "lock_y": lock_y,
                        "lock_yaw": lock_yaw
                    }
                # 3. 执行命中检测（仅命中时才扣血）
                has_hit = check_fire_hit(pid)
                with fire_lock:
                    fire_hit_results[pid] = has_hit
                # 4. 设置开火动画（无论是否命中都播放）
                with state_lock:
                    player_states[pid]["ani_id"] = 2
                log(f"玩家{pid}按住开火，定格位置({lock_x:.1f},{lock_y:.1f})，转向{lock_yaw:.1f}°")
                return

            # 处理开火松开（k|nf）
            elif key_code == "nf":
                with fire_lock:
                    # 1. 解除开火锁定
                    if pid in fire_lock_states:
                        fire_lock_states[pid]["is_locked"] = False
                log(f"玩家{pid}松开开火，恢复移动/转向权限")
                return

            # 处理普通移动按键
            key_name, is_pressed = KEY_PROTOCOL_MAP[key_code]
            with state_lock:
                player_key_states[pid][key_name] = is_pressed
            log(f"玩家{pid}按键更新：{key_name}={'按下' if is_pressed else '松开'}")

        # 处理转向协议（m|rotate_code）
        elif msg.startswith("m|"):
            parts = msg.split("|", 2)
            if len(parts) < 2 or parts[1].strip() == "":
                log_error(f"玩家{pid}转向协议格式错误：{msg}")
                return
            rotate_code = parts[1].strip()
            if rotate_code not in ["l", "r", "s"]:
                log_error(f"玩家{pid}未知转向码：{rotate_code}")
                return
            with state_lock:
                player_rotate_states[pid] = rotate_code
            log(f"玩家{pid}转向更新：{'左转向' if rotate_code == 'l' else '右转向' if rotate_code == 'r' else '停止转向'}")

        else:
            log_error(f"玩家{pid}无效协议：{msg}（支持：k|xx/m|xx）")
    except Exception as e:
        log_error(f"解析玩家{pid}协议失败：{str(e)}")


# ===================== 客户端处理（新增掉线发送死亡协议）=====================
def handle_client(client_sock, client_addr):
    """处理单个客户端连接（断开时清理开火/命中状态+发送死亡协议）"""
    global next_player_id
    player_id = 0
    msg_count = 0
    last_tick_time = time.time()
    sock_valid = True
    client_ip, client_port = client_addr

    try:
        # Socket配置
        client_sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, SEND_BUFFER_SIZE)
        client_sock.setblocking(False)

        # 分配玩家ID
        with client_lock:
            player_id = next_player_id
            next_player_id += 1
            if client_sock not in client_id_map:
                client_id_map[client_sock] = player_id
            if client_sock not in client_sockets:
                client_sockets.append(client_sock)
        init_player(player_id)

        # 发送ID给客户端
        if safe_send(client_sock, f"ID|{player_id}"):
            log(f"客户端[{client_ip}:{client_port}]连接成功，分配玩家ID={player_id}")
        else:
            log_error(f"客户端[{client_ip}:{client_port}]分配ID后发送失败，断开连接")
            sock_valid = False
            return

        # 循环接收消息
        while game_running and sock_valid:
            current_time = time.time()
            if current_time - last_tick_time >= GAME_TICK_INTERVAL:
                msg_count = 0
                last_tick_time = current_time
            if msg_count >= MAX_MSG_PER_TICK:
                time.sleep(0.001)
                continue

            try:
                data = client_sock.recv(1024)
                if not data:
                    log(f"客户端[{client_ip}:{client_port}]（ID={player_id}）主动断开连接")
                    break
                msg = data.decode('utf-8', errors='replace').strip()
                if msg:
                    parse_client_protocol(player_id, msg, client_sock)
                    msg_count += 1
            except BlockingIOError:
                time.sleep(0.001)
            except socket.error as e:
                if hasattr(e, 'winerror') and e.winerror == 10038:
                    log_error(f"客户端[{client_ip}:{client_port}]（ID={player_id}）套接字失效，立即清理")
                else:
                    log_error(f"接收客户端[{client_ip}:{client_port}]（ID={player_id}）消息异常：{str(e)}")
                sock_valid = False
                break
            except Exception as e:
                log_error(f"处理客户端[{client_ip}:{client_port}]（ID={player_id}）消息异常：{str(e)}")
                time.sleep(0.001)

    except Exception as e:
        log_error(f"客户端[{client_ip}:{client_port}]（ID={player_id}）连接异常：{str(e)}")
    finally:
        # 清理资源（含开火/命中状态 + 新增发送死亡协议）
        try:
            log(f"开始清理客户端[{client_ip}:{client_port}]（ID={player_id}）资源")

            # 新增：玩家掉线发送死亡协议
            if player_id != 0 and not player_death_flag.get(player_id, False):
                broadcast_death_protocol(player_id)

            # 1. 清理Socket映射
            with client_lock:
                if client_sock in client_sockets:
                    client_sockets.remove(client_sock)
                client_id_map.pop(client_sock, None)
            # 2. 清理玩家状态
            with state_lock:
                player_states.pop(player_id, None)
                player_key_states.pop(player_id, None)
                player_rotate_states.pop(player_id, None)
            with fire_lock:
                fire_lock_states.pop(player_id, None)
                hit_players.pop(player_id, None)
                fire_hit_results.pop(player_id, None)
            with score_lock:
                # 可选：保留得分记录，若需清空则取消注释
                # player_scores.pop(player_id, None)
                pass
            # 清理死亡标记
            player_death_flag.pop(player_id, None)
            # 3. 清理统计信息
            with stats_lock:
                command_stats.pop(player_id, None)
            # 4. 关闭Socket
            client_sock.close()
            log(f"客户端[{client_ip}:{client_port}]（ID={player_id}）资源清理完成")
        except Exception as e:
            log_error(f"清理客户端[{client_ip}:{client_port}]（ID={player_id}）资源失败：{str(e)}")


# ===================== 游戏主循环（修改日志提示）=====================
def build_broadcast_msg():
    """构建广播消息（包含ani=2/3和扣血后的HP）"""
    try:
        with client_lock:
            online_pids = list(client_id_map.values())
        with state_lock:
            msg_parts = ["pos", str(len(online_pids))]
            for pid in online_pids:
                if pid not in player_states:
                    log_error(f"广播时玩家{pid}状态不存在，跳过")
                    continue
                s = player_states[pid]
                # 消息格式：pos|玩家数|ID|x|y|z|roll|pitch|yaw|hp|ani_id
                msg_parts.extend([
                    str(pid),
                    f"{s['x']:.1f}", f"{s['y']:.1f}", f"{s['z']:.1f}",
                    f"{s['roll']:.1f}", f"{s['pitch']:.1f}", f"{s['yaw']:.1f}",
                    f"{s['hp']:.0f}", f"{s['ani_id']:.0f}"
                ])
        broadcast_msg = "|".join(msg_parts)
        log(f"广播状态：{len(online_pids)}个玩家，消息长度：{len(broadcast_msg)}字节")
        return broadcast_msg
    except Exception as e:
        log_error(f"构建广播消息失败：{str(e)}")
        return "pos|0"


def safe_send(sock, msg):
    """安全发送消息"""
    try:
        data = msg.encode('utf-8')
        total_sent = 0
        data_len = len(data)
        while total_sent < data_len and game_running:
            sent = sock.send(data[total_sent:])
            if sent == 0:
                log_error(f"Socket发送失败：连接已断开（未发送字节：{data_len - total_sent}）")
                return False
            total_sent += sent
        log(f"Socket发送成功：{total_sent}/{data_len}字节，消息：{msg[:50]} {'...' if len(msg) > 50 else ''}")
        return True
    except Exception as e:
        log_error(f"Socket发送异常：{str(e)}")
        return False


def game_main_loop():
    """游戏主循环（同步开火/受伤状态）"""
    # 核心修改：日志提示从10帧/秒改为20帧/秒
    log(f"游戏主循环启动 → 20帧/秒，基于射线+球体碰撞的命中检测（射线长度：{FIRE_RAY_LENGTH}，碰撞半径：{PLAYER_COLLISION_RADIUS}）")
    while game_running:
        try:
            with client_lock:
                has_clients = len(client_sockets) > 0
            if not has_clients:
                time.sleep(GAME_TICK_INTERVAL)
                continue

            # 1. 获取在线玩家ID
            with client_lock:
                online_pids = list(client_id_map.values())
            # 2. 更新所有玩家状态（移动/转向/开火/受伤）
            for pid in online_pids:
                update_player_movement(pid)
                update_player_rotation(pid)

            # 3. 构建并广播状态消息
            broadcast_msg = build_broadcast_msg()
            dead_sockets = []
            with client_lock:
                for sock in list(client_sockets):
                    if not safe_send(sock, broadcast_msg):
                        dead_sockets.append(sock)

            # 4. 清理失效连接
            if dead_sockets:
                with client_lock:
                    for sock in dead_sockets:
                        if sock in client_sockets:
                            client_sockets.remove(sock)
                        client_id_map.pop(sock, None)
                        try:
                            sock.close()
                        except:
                            pass
                log(f"清理{len(dead_sockets)}个失效客户端连接，当前在线：{len(client_sockets)}")

            time.sleep(GAME_TICK_INTERVAL)
        except Exception as e:
            log_error(f"游戏主循环异常：{str(e)}")
            time.sleep(0.1)


# ===================== 死连接检测（无修改）=====================
def check_dead_connections():
    """检测并清理客户端死连接"""
    while game_running:
        time.sleep(CHECK_DEAD_CONN_INTERVAL)
        with client_lock:
            if len(client_sockets) == 0:
                continue

        dead_sockets = []
        with client_lock:
            for sock in list(client_sockets):
                try:
                    sock.recv(0)
                except Exception as e:
                    log_error(f"Socket有效性检测失败：{str(e)}，标记为死连接")
                    dead_sockets.append(sock)

        if dead_sockets:
            with client_lock:
                for sock in dead_sockets:
                    if sock in client_sockets:
                        client_sockets.remove(sock)
                    client_id_map.pop(sock, None)
                    try:
                        sock.close()
                    except:
                        pass
            log(f"死连接检测：清理{len(dead_sockets)}个客户端连接，当前在线：{len(client_sockets)}")


# ===================== 服务器启动（无核心修改）=====================
def start_server():
    """启动服务器，监听8888端口"""
    global game_running
    server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, SEND_BUFFER_SIZE)

    try:
        server_sock.bind(('0.0.0.0', 8888))
        server_sock.listen(10)
        log(f"🚀 TCP服务器启动成功 → 监听 0.0.0.0:8888")
        log(f"✅ 帧率配置：20帧/秒（每帧间隔0.05秒）")
        log(f"✅ 碰撞参数：射线长度={FIRE_RAY_LENGTH}，玩家碰撞半径={PLAYER_COLLISION_RADIUS}，扣血={FIRE_DAMAGE}HP/帧")
        log(f"✅ 协议配置：得分广播间隔{SCORE_BROADCAST_INTERVAL}秒，每次命中得分+{SCORE_PER_HIT}")
    except Exception as e:
        log_error(f"服务器启动失败：{str(e)}")
        sys.exit(1)

    # 启动子线程（新增得分协议广播线程）
    threading.Thread(target=game_main_loop, daemon=True, name="GameMainLoop").start()
    threading.Thread(target=check_dead_connections, daemon=True, name="DeadConnCheck").start()
    threading.Thread(target=print_command_and_state_stats, daemon=True, name="StatsPrint").start()
    threading.Thread(target=send_score_protocol_loop, daemon=True, name="ScoreBroadcastLoop").start()

    # 接收客户端连接
    try:
        log(f"⏳ 等待客户端连接...")
        while game_running:
            client_sock, client_addr = server_sock.accept()
            threading.Thread(
                target=handle_client,
                args=(client_sock, client_addr),
                daemon=True,
                name=f"ClientHandler_{client_addr[0]}:{client_addr[1]}"
            ).start()
    except KeyboardInterrupt:
        log("⚠️ 收到关闭信号，正在停止服务器...")
        game_running = False
    finally:
        server_sock.close()
        log("🔌 服务器已完全关闭")


if __name__ == "__main__":
    try:
        start_server()
    except Exception as e:
        log_error(f"服务器启动失败：{str(e)}")
        sys.exit(1)