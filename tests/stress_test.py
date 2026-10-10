import socket
import struct
import json
import time
import sys

# ---------- 协议常量 ----------

MAGIC = 0x434C
VERSION = 0x01

# 消息类型
MSG_LOGIN = 0x01
MSG_LOGOUT = 0x02
MSG_LIST = 0x03
MSG_BROADCAST = 0x04
MSG_PRIVATE = 0x05
MSG_LOGIN_RESP = 0x81
MSG_LIST_RESP = 0x82
MSG_BROADCAST_RECV = 0x83
MSG_PRIVATE_RECV = 0x84
MSG_SYSTEM = 0x85
MSG_ERROR = 0x86

# ---------- 协议工具 ----------

def send_msg(sock, msg_type, payload):
    """按协议帧格式发送一条消息。"""
    data = json.dumps(payload).encode('utf-8')
    header = struct.pack('!HBBI', MAGIC, VERSION, msg_type, len(data))
    sock.sendall(header + data)


def recv_exact(sock, n):
    """精确读取 n 字节。对端关闭时抛 ConnectionError。"""
    buf = b''
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("connection closed")
        buf += chunk
    return buf


def recv_msg(sock):
    """读取一条完整消息，返回 (type, payload_dict)。"""
    header = recv_exact(sock, 8)
    magic, version, msg_type, length = struct.unpack('!HBBI', header)
    payload_bytes = recv_exact(sock, length) if length > 0 else b''
    payload = json.loads(payload_bytes.decode('utf-8')) if payload_bytes else {}
    return msg_type, payload


def connect_and_login(host, port, username):
    """连接服务端并登录，返回已登录的 socket。"""
    s = socket.create_connection((host, port))
    send_msg(s, MSG_LOGIN, {"username": username})
    t, p = recv_msg(s)
    if t != MSG_LOGIN_RESP or not p.get("ok"):
        s.close()
        raise RuntimeError(f"login failed for {username}: {p}")
    return s


def recv_until(sock, expected_types, verbose=True):
    """循环读取消息，跳过不关心的类型（如 SYSTEM），直到收到期望类型之一。"""
    while True:
        t, p = recv_msg(sock)
        if t in expected_types:
            return t, p
        if verbose:
            print(f"    (skipped type=0x{t:02X} payload={p})")

# 1MiB 消息测试
def test_large_message(host, port):
    print("=== Task 4: 1 MiB message ===")

    try:
        a = connect_and_login(host, port, "alice")
        time.sleep(0.2)
        b = connect_and_login(host, port, "bob")
        time.sleep(0.2)

        content = "A" * (1024 * 1024)  # 1 MiB
        send_msg(b, MSG_BROADCAST, {"content": content})

        # alice 应收到广播（跳过可能存在的 SYSTEM 通知）
        t, p = recv_until(a, [MSG_BROADCAST_RECV])
        if t != MSG_BROADCAST_RECV:
            print(f"[FAIL] expected BROADCAST_RECV, got type=0x{t:02X}")
            return False
        received = p.get("content", "")
        if len(received) != len(content):
            print(f"[FAIL] content length mismatch: expected {len(content)}, got {len(received)}")
            return False
        if received != content:
            print(f"[FAIL] content mismatch")
            return False
        print(f"[PASS] received 1 MiB content correctly")
        a.close()
        b.close()
        return True
    except Exception as e:
        print(f"[FAIL] exception: {e}")
        return False

# 2MiB (超限)消息测试
def test_oversize_message(host, port):
    print("=== Task 5: oversize message ===")

    try:
        b = connect_and_login(host, port, "carol")  
        time.sleep(0.2)

        content = "B" * (2 * 1024 * 1024)  # 2 MiB
        send_msg(b, MSG_BROADCAST, {"content": content})

        t, p = recv_until(b, [MSG_ERROR])
        if t != MSG_ERROR:
            print(f"[FAIL] expected ERROR, got type=0x{t:02X}")
            return False
        if p.get("code") != 1005:
            print(f"[FAIL] expected code 1005, got {p.get('code')}")
            return False
        print(f"[PASS] got ERROR 1005: {p.get('message')}")
        b.close()
        return True
    except Exception as e:
        print(f"[FAIL] exception: {e}")
        return False

# 消息分三次发送
def test_fragmented_send(host, port):
    print("=== Task 7: fragmented send ===")

    try:
        b = connect_and_login(host, port, "dave")  
        time.sleep(0.2)
        # 构造一条消息
        payload = json.dumps({"content": "fragmented hello"}).encode('utf-8')
        header = struct.pack('!HBBI', MAGIC, VERSION, MSG_BROADCAST, len(payload))
        frame = header + payload
        # 分成三段
        total = len(frame)
        part1 = frame[:total // 3]
        part2 = frame[total // 3: 2 * total // 3]
        part3 = frame[2 * total // 3:]
        # 分三次间隔发送
        b.sendall(part1)
        time.sleep(0.5)
        b.sendall(part2)
        time.sleep(0.5)
        b.sendall(part3)
        # bob 应收到自己的广播回显
        t, p = recv_until(b, [MSG_BROADCAST_RECV])
        if t != MSG_BROADCAST_RECV:
            print(f"[FAIL] expected BROADCAST_RECV, got type=0x{t:02X}")
            return False
        if p.get("content") != "fragmented hello":
            print(f"[FAIL] content mismatch: {p.get('content')}")
            return False
        print(f"[PASS] fragmented message reconstructed")
        b.close()
        return True
    except Exception as e:
        print(f"[FAIL] exception: {e}")
        return False

# 坏数据(MAGIC, VERSON, LENGTH, JSON)
def test_bad_data(host, port):
    print("=== Task 8: bad data ===")
    results = []

    # 8.1 错误 MAGIC
    try:
        s = socket.create_connection((host, port))
        s.sendall(b'\xFF\xFF' + struct.pack('!BBI', VERSION, MSG_LOGIN, 0))
        time.sleep(0.3)
        s.settimeout(2)
        data = s.recv(1024)
        closed = (data == b'')
        results.append(("bad magic", closed))
        s.close()
    except Exception as e:
        results.append(("bad magic", False))
        print(f"    exception: {e}")

    # 8.2 错误 VERSION
    try:
        s = socket.create_connection((host, port))
        s.sendall(struct.pack('!HBBI', MAGIC, 0xFF, MSG_LOGIN, 0))
        time.sleep(0.3)
        s.settimeout(2)
        data = s.recv(1024)
        closed = (data == b'')
        results.append(("bad version", closed))
        s.close()
    except Exception as e:
        results.append(("bad version", False))
        print(f"    exception: {e}")

    # 8.3 天文数字 LENGTH
    try:
        s = socket.create_connection((host, port))
        s.sendall(struct.pack('!HBBI', MAGIC, VERSION, MSG_LOGIN, 0xFFFFFFFF))
        time.sleep(0.3)
        s.settimeout(2)
        data = s.recv(1024)
        closed = (data == b'')
        results.append(("huge length", closed))
        s.close()
    except Exception as e:
        results.append(("huge length", False))
        print(f"    exception: {e}")

    # 8.4 非法 JSON
    try:
        s = socket.create_connection((host, port))
        bad_payload = b"this is not json"
        header = struct.pack('!HBBI', MAGIC, VERSION, MSG_LIST, len(bad_payload))
        s.sendall(header + bad_payload)
        time.sleep(0.3)
        s.settimeout(2)
        data = s.recv(1024)
        closed = (data == b'')
        results.append(("bad json", closed))
        s.close()
    except Exception as e:
        results.append(("bad json", False))
        print(f"    exception: {e}")

    # 8.5 类型不认识（需要登录）
    try:
        s = connect_and_login(host, port, "eve")
        send_msg(s, 0x55, {"foo": "bar"})
        t, p = recv_until(s, [MSG_ERROR])
        results.append(("unknown type", t == MSG_ERROR and p.get("code") == 1007))
        s.close()
    except Exception as e:
        results.append(("unknown type", False))
        print(f"    exception: {e}")

    all_pass = True
    for name, ok in results:
        status = "PASS" if ok else "FAIL"
        print(f"[{status}] {name}")
        if not ok:
            all_pass = False
    return all_pass

def main():
    host = "127.0.0.1"
    port = 9000

    # 检查服务端是否可达
    try:
        s = socket.create_connection((host, port), timeout=2)
        s.close()
    except Exception as e:
        print(f"Cannot connect to server at {host}:{port} - {e}")
        sys.exit(1)

    results = []
    results.append(("Task 4: 1 MiB message", test_large_message(host, port)))
    time.sleep(0.5)
    results.append(("Task 5: oversize message", test_oversize_message(host, port)))
    time.sleep(0.5)
    results.append(("Task 7: fragmented send", test_fragmented_send(host, port)))
    time.sleep(0.5)
    results.append(("Task 8: bad data", test_bad_data(host, port)))

    print()
    print("=== Summary ===")
    all_pass = True
    for name, ok in results:
        status = "PASS" if ok else "FAIL"
        print(f"[{status}] {name}")
        if not ok:
            all_pass = False

    print()
    if all_pass:
        print("ALL STRESS TESTS PASSED")
        sys.exit(0)
    else:
        print("SOME TESTS FAILED")
        sys.exit(1)


if __name__ == "__main__":
    main()