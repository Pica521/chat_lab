import struct
import json
import socket

MAGIC = 0x434C
VERSION = 0x01

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

g_failed = 0


def send_msg(sock, msg_type, payload):
    data = json.dumps(payload).encode('utf-8')
    header = struct.pack('!HBBI', MAGIC, VERSION, msg_type, len(data))
    sock.sendall(header + data)


def recv_exact(sock, n):
    buf = b''
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("connection closed")
        buf += chunk
    return buf


def recv_msg(sock):
    header = recv_exact(sock, 8)
    magic, version, msg_type, length = struct.unpack('!HBBI', header)
    payload = recv_exact(sock, length) if length > 0 else b''
    return msg_type, json.loads(payload.decode('utf-8'))


def recv_until(sock, expected_types, verbose=True):
    """读取消息，跳过所有 SYSTEM 通知，直到收到 expected_types 里的类型。"""
    while True:
        t, p = recv_msg(sock)
        if t in expected_types:
            return t, p
        if verbose:
            print(f"    (skipped type=0x{t:02X} payload={p})")


def expect(name, cond, extra=""):
    global g_failed
    status = "PASS" if cond else "FAIL"
    print(f"[{status}] {name} {extra}")
    if not cond:
        g_failed += 1


def main():
    global g_failed
    g_failed = 0

    a = socket.create_connection(("127.0.0.1", 9000))
    b = socket.create_connection(("127.0.0.1", 9000))

    # 1. A 登录 alice
    send_msg(a, MSG_LOGIN, {"username": "alice"})
    t, p = recv_until(a, [MSG_LOGIN_RESP])
    expect("1. A login alice", t == MSG_LOGIN_RESP and p.get("ok") is True, p)

    # 2. B 登录 bob
    send_msg(b, MSG_LOGIN, {"username": "bob"})
    t, p = recv_until(b, [MSG_LOGIN_RESP])
    expect("2. B login bob", t == MSG_LOGIN_RESP and p.get("ok") is True, p)

    # 3. C 用新连接尝试登录 bob（应失败 1001）
    c = socket.create_connection(("127.0.0.1", 9000))
    send_msg(c, MSG_LOGIN, {"username": "bob"})
    t, p = recv_until(c, [MSG_ERROR])
    expect("3. C login bob fails 1001",
           t == MSG_ERROR and p.get("code") == 1001, p)
    c.close()

    # 4. A 发 LIST（跳过 SYSTEM 通知）
    send_msg(a, MSG_LIST, {})
    t, p = recv_until(a, [MSG_LIST_RESP])
    users = p.get("users", [])
    expect("4. A list contains alice & bob",
           "alice" in users and "bob" in users, users)

    # 5. A 群发
    send_msg(a, MSG_BROADCAST, {"content": "hello everyone"})
    t, p = recv_until(a, [MSG_BROADCAST_RECV])
    expect("5. A receives broadcast echo",
           p.get("from") == "alice"
           and p.get("content") == "hello everyone", p)

    t, p = recv_until(b, [MSG_BROADCAST_RECV])
    expect("6. B receives broadcast",
           p.get("from") == "alice", p)

    # 6. A 私聊 B
    send_msg(a, MSG_PRIVATE, {"to": "bob", "content": "hi bob"})
    t, p = recv_until(b, [MSG_PRIVATE_RECV])
    expect("7. B receives private",
           p.get("from") == "alice"
           and p.get("content") == "hi bob", p)

    # 7. A 私聊 charlie（不在线）
    send_msg(a, MSG_PRIVATE, {"to": "charlie", "content": "hello?"})
    t, p = recv_until(a, [MSG_ERROR])
    expect("8. A private to offline fails 1004",
           p.get("code") == 1004, p)

    # 8. A 登出
    send_msg(a, MSG_LOGOUT, {})
    t, p = recv_until(a, [MSG_SYSTEM])
    expect("9. A logout gets bye",
           p.get("content") == "bye", p)

    # B 可能收到 "alice has left"
    t, p = recv_until(b, [MSG_SYSTEM])
    expect("10. B notified alice left",
           "alice" in p.get("content", ""), p)

    # 9. 未登录客户端 D 发 LIST
    d = socket.create_connection(("127.0.0.1", 9000))
    send_msg(d, MSG_LIST, {})
    t, p = recv_until(d, [MSG_ERROR])
    expect("11. D not logged in fails 1003",
           p.get("code") == 1003, p)
    d.close()

    # 10. B 发未知类型
    send_msg(b, 0x55, {})
    t, p = recv_until(b, [MSG_ERROR])
    expect("12. B unknown type fails 1007",
           p.get("code") == 1007, p)

    a.close()
    b.close()

    print()
    if g_failed == 0:
        print("ALL TESTS PASSED")
    else:
        print(f"{g_failed} TESTS FAILED")


if __name__ == "__main__":
    main()