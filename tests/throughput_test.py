import socket
import struct
import json
import time
import threading
import sys

# 协议常量
MAGIC = 0x434C
VERSION = 0x01

MSG_LOGIN = 0x01
MSG_BROADCAST = 0x04
MSG_PRIVATE = 0x05
MSG_LOGIN_RESP = 0x81
# 只关心数据流

# 基础函数
# 按协议帧格式发送一条消息
def send_msg(sock, msg_type, payload):
    data = json.dumps(payload).encode('utf-8')
    header = struct.pack('!HBBI', MAGIC, VERSION, msg_type, len(data))
    sock.sendall(header + data)

# 精确读取 n 字节。对端关闭时抛出异常
def recv_exact(sock, n):
    
    buf = b''
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("connection closed")
        buf += chunk
    return buf

# 连接服务端并登录，返回已登录的 socket
def connect_and_login(host, port, username):
    
    s = socket.create_connection((host, port))
    # 设置 TCP_NODELAY 减少延迟
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    
    send_msg(s, MSG_LOGIN, {"username": username})
    
    # 解析登录响应
    header = recv_exact(s, 8)
    magic, version, msg_type, length = struct.unpack('!HBBI', header)
    payload = recv_exact(s, length) if length > 0 else b''
    p = json.loads(payload.decode('utf-8'))
    
    if msg_type != MSG_LOGIN_RESP or not p.get("ok"):
        s.close()
        raise RuntimeError(f"login failed for {username}: {p}")
    return s

# 接收端类
class Receiver:
    def __init__(self, sock):
        self.sock = sock
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)
        
        # 统计指标
        self.bytes_received = 0
        self.msgs_received = 0
        self.error_occurred = False

    def start(self):
        self.thread.start()

    def _run(self):
        try:
            while not self.stop_event.is_set():
                # 读 8 字节头
                header = recv_exact(self.sock, 8)
                magic, version, msg_type, length = struct.unpack('!HBBI', header)
                
                # 跳过 payload, 只看 LENGTH
                if length > 0:
                    recv_exact(self.sock, length)
                
                # 累加统计
                self.bytes_received += (8 + length)  # 考虑头长
                self.msgs_received += 1
                
        except ConnectionError:
            # 正常断开(测试结束时主动 close)
            pass
        except Exception as e:
            self.error_occurred = True
            # 不打印，避免干扰，由主线程去检查
        finally:
            # 线程退出前，尝试关闭 socket 触发主线程的清理
            try:
                self.sock.close()
            except:
                pass
    
    # 主线程调用：通知退出、等待线程结束、返回数据
    def stop_and_get_stats(self):
        
        self.stop_event.set()
        # 关闭 socket 唤醒阻塞在 recv 的线程
        try:
            self.sock.shutdown(socket.SHUT_RDWR)  # 强制打断阻塞的 recv，避免死锁
        except:
            pass
        self.thread.join(timeout=2.0)
        
        return self.bytes_received, self.msgs_received, self.error_occurred

# 构造一个完整的协议帧
def build_frame(msg_type, target_size, target_user=None):
    # 头长
    header_size = 8

    # 群发
    if msg_type == MSG_BROADCAST:
        # JSON 的外壳长度： {"content":""} 是 14 字节
        # 实际 content 的字符串长度 = target_size - header_size - 14
        content_len = target_size - header_size - 14
        content = "A" * content_len
        # 不要空格
        data = json.dumps({"content": content}, separators=(',', ':')).encode('utf-8')
    
    # 私发
    elif msg_type == MSG_PRIVATE:
        # JSON 外壳长度取决于用户名长度，先构造空信息获取
        empty_data = json.dumps({"to": target_user, "content": ""}, separators=(',', ':'))
        outer_len = len(empty_data)
        content_len = target_size - header_size - outer_len
        content = 'A' * content_len
        data = json.dumps({"to": target_user, "content": content}, separators=(',', ':')).encode('utf-8')
    else:
        raise ValueError("invalid msg_type")
       
    # 拼成完整的帧
    header = struct.pack('!HBBI', MAGIC, VERSION, msg_type, len(data))
    return header + data

"""
运行一组测试场景。
mode: "private" 或 "broadcast"
"""
def run_scenario(host, port, msg_size, mode, num_receivers, duration=10, warmup=2):
    
    print(f"--- Running: Size={msg_size}B, Mode={mode}, Receivers={num_receivers} ---")

    msg_type = MSG_BROADCAST if mode == "broadcast" else MSG_PRIVATE
    target_user = f"recv_{mode}_0"

    # 1. 启动接收者
    receivers = []
    for i in range(num_receivers):
        try:
            s = connect_and_login(host, port, f"recv_{mode}_{i}")
            r = Receiver(s)
            receivers.append(r)
        except Exception as e:
            print(f"[ERROR] Failed to login receiver: {e}")
            return None
            
    # 2. 启动发送者
    try:
        sender_sock = connect_and_login(host, port, f"talker_{mode}")
        sender_drainer = Receiver(sender_sock)  # 挂一个接收器读掉群发回显
        sender_drainer.start()
    except Exception as e:
        print(f"[ERROR] Failed to login sender: {e}")
        return None
        
    # 等半秒，让系统通知发完
    time.sleep(0.5)
    
    # 3. 启动接收线程
    for r in receivers:
        r.start()
    
    try:
        frame = build_frame(msg_type, msg_size, target_user)
    except Exception as e:
        print(f"[ERROR] Failed to build frame: {e}")
        return None

    # 4. 预热
    start_warmup = time.time()
    while time.time() - start_warmup < warmup:
        sender_sock.sendall(frame)

    # 5. 清除预热数据
    for r in receivers:
        r.bytes_received = 0
        r.msgs_received = 0
        r.error_occurred = False

    # 6. 正式话痨测试
    start_time = time.time()
    send_count = 0
    send_bytes = 0

    try:
        while time.time() - start_time < duration:
            sender_sock.sendall(frame)
            send_count += 1
            send_bytes += len(frame)
    except Exception as e:
        print(f"[ERROR] Send failed: {e}")

    end_time = time.time()
    elapsed = end_time - start_time

    # 7. 断开连接
    try:
        sender_sock.close()
    except:
        pass

    # 收集接收端数据
    total_recv_bytes = 0
    total_recv_msgs = 0
    for r in receivers:
        b, m, err = r.stop_and_get_stats()
        total_recv_bytes += b
        total_recv_msgs += m
        if err:
            print("    [WARN] A receiver encountered an error")

    # 8. 计算
    send_qps = send_count / elapsed
    send_mib = (send_bytes / elapsed) / (1024 * 1024)
    send_mbps = send_mib * 8
    # 接收端视角：所有接收者实际收到的数据总和
    total_recv_mib = (total_recv_bytes / elapsed) / (1024 * 1024)
    total_recv_mbps = total_recv_mib * 8
    # 单个接收者的平均速率（群发时，每个接收者分担的流量）
    avg_recv_mib = total_recv_mib / num_receivers if num_receivers > 0 else 0

    print(f"  Duration:    {elapsed:.2f}s")
    print(f"  Sender QPS:  {send_qps:.2f} msg/s")
    print(f"  [Sender] Inject Rate: {send_mib:.2f} MiB/s  ({send_mbps:.2f} Mbps)")
    print(f"  [Server] Egress Rate: {total_recv_mib:.2f} MiB/s  ({total_recv_mbps:.2f} Mbps)  (Total received by all)")

    if num_receivers > 1:
        print(f"  [Client] Recv Rate per user: {avg_recv_mib:.2f} MiB/s")

    print(f"  Total Msgs Received: {total_recv_msgs}")

    return {
        "mode": mode,
        "msg_size": msg_size,
        "qps": send_qps,
        "send_mbps": send_mbps,
        "recv_mbps": total_recv_mbps,
        "elapsed": elapsed
    }

    
def main():
    host = "127.0.0.1"
    port = 9000

    if len(sys.argv) >= 2:
        host = sys.argv[1]
    if len(sys.argv) >= 3:
        port = int(sys.argv[2])

    print(f" Starting Throughput Test against {host}:{port}")

    # 定义 6 组场景
    # 格式: (消息大小 Bytes, 模式, 接收者数量)
    scenarios = [
        (1 * 1024, "private", 1),       # A1: 1 KiB 私发
        (64 * 1024, "private", 1),      # A2: 64 KiB 私发
        (1 * 1024 * 1024, "private", 1),# A3: 1 MiB 私发
        (1 * 1024, "broadcast", 3),     # B1: 1 KiB 3人群发
        (64 * 1024, "broadcast", 3),    # B2: 64 KiB 3人群发
        (1 * 1024 * 1024, "broadcast", 3) # B3: 1 MiB 3人群发
    ]

    results = []

    for msg_size, mode, num_recv in scenarios:
        res = run_scenario(host, port, msg_size, mode, num_recv, duration=10, warmup=2)
        if res:
            results.append(res)
        else:
            print("[WARN] Scenario failed.")
        
        # 场景间隔，让服务端清理上一个场景的线程和连接
        print("Waiting 2s for server to clean up...")
        time.sleep(2)
        print("")

    # 打印汇总表格 (Markdown 格式)
    print("\n"+"Summary Table (Markdown)"+"\n")
    print("| 场景 | 大小 | 模式 | 接收者 | QPS | 发送速率(MiB/s) | 发送速率(Mbps) | 服务端吞吐量(MiB/s) | 服务端吞吐量(Mbps) |")
    print("|---|---|---|---|---|---|---|---|---|")
    
    for i, r in enumerate(results):
        size_str = f"{r['msg_size']//1024} KiB" if r['msg_size'] < 1024*1024 else f"{r['msg_size']//(1024*1024)} MiB"
        mode_str = "私发" if r["mode"] == "private" else "群发"
        recv_count = 1 if r["mode"] == "private" else 3
        
        send_mbps = r['send_mbps']
        send_mib = send_mbps / 8
        recv_mbps = r['recv_mbps']
        recv_mib = recv_mbps / 8
        print(f"| {i+1} | {size_str} | {mode_str} | {recv_count} | {r['qps']:.2f} | {send_mib:.2f} | {send_mbps:.2f} | {recv_mib:.2f} | {recv_mbps:.2f} |")
        
    print("\n")

if __name__ == "__main__":
    main()