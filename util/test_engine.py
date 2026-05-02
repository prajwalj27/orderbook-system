import socket
import threading
import time

# ── helpers ──────────────────────────────────────────────────────────────

def listen_trades(conn):
    """Background thread — prints every trade event arriving on port 7002."""
    while True:
        try:
            data = conn.recv(4096).decode()
            if not data:
                break
            for line in data.strip().split('\n'):
                if line:
                    print(f"    [TRADE]  {line}")
        except:
            break

def send_order(conn, msg, label):
    """Send one FIX message, wait briefly, print the execution report."""
    print(f"\n  {label}")
    print(f"    SEND:  {msg}")
    conn.sendall((msg + '\n').encode())
    time.sleep(0.1)
    conn.settimeout(0.5)
    try:
        response = conn.recv(4096).decode().strip()
        for line in response.split('\n'):
            if line:
                print(f"    RECV:  {line}")
    except socket.timeout:
        pass
    time.sleep(0.1)

# ── connect ───────────────────────────────────────────────────────────────

trade_conn = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
trade_conn.connect(('localhost', 7002))
print("Connected to port 7002 (trade feed)")

order_conn = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
order_conn.connect(('localhost', 7001))
print("Connected to port 7001 (order gateway)")

threading.Thread(target=listen_trades, args=(trade_conn,), daemon=True).start()
time.sleep(0.1)

# ── scenarios ─────────────────────────────────────────────────────────────

print("\n" + "="*60)
print(" SCENARIO 1 — LIMIT orders that rest (no match)")
print("="*60)
# Book is empty. Both orders sit in the book with no counterpart.
send_order(order_conn, "35=D|11=ORD001|54=1|44=150.00|38=100",
           "LIMIT BUY  100 @ $150.00  →  expect ACK (39=0)")

send_order(order_conn, "35=D|11=ORD002|54=2|44=151.00|38=100",
           "LIMIT SELL 100 @ $151.00  →  expect ACK (39=0)")
# Book state: bids={150.00:[ORD001]}, asks={151.00:[ORD002]}

print("\n" + "="*60)
print(" SCENARIO 2 — Crossing LIMIT order → full fill")
print("="*60)
# Incoming SELL @ $150.00 crosses with BUY @ $150.00.
# Fill executes at the resting order's price ($150.00).
send_order(order_conn, "35=D|11=ORD003|54=2|44=150.00|38=100",
           "LIMIT SELL 100 @ $150.00  →  expect FILLED (39=2) + TRADE event")
# Book state: bids={}, asks={151.00:[ORD002]}

print("\n" + "="*60)
print(" SCENARIO 3 — MARKET order")
print("="*60)
# MARKET BUY has no price — matches at whatever the best ask is ($151.00).
send_order(order_conn, "35=D|11=ORD004|54=1|38=50",
           "MARKET BUY 50           →  expect FILLED (39=2) + TRADE event @ $151.00")
# ORD002 had 100 shares, 50 filled → 50 remaining
# Book state: bids={}, asks={151.00:[ORD002(50 left)]}

print("\n" + "="*60)
print(" SCENARIO 4 — Partial fill, remainder rests")
print("="*60)
# BUY 200 @ $152.00 crosses with the remaining 50 of ORD002.
# 50 shares fill, 150 remain → order is PARTIAL and rests in book.
send_order(order_conn, "35=D|11=ORD005|54=1|44=152.00|38=200",
           "LIMIT BUY  200 @ $152.00  →  expect PARTIAL (39=1), 150 rests in book")
# Book state: bids={152.00:[ORD005(150 left)]}, asks={}

print("\n" + "="*60)
print(" SCENARIO 5 — IOC order")
print("="*60)
# IOC SELL 300 @ $152.00 — matches 150 against ORD005, remainder of 150 is cancelled.
# Never rests in the book regardless of how much fills.
send_order(order_conn, "35=D|11=ORD006|54=2|44=152.00|38=300|40=3",
           "IOC  SELL  300 @ $152.00  →  expect CANCEL (39=4), 150 filled + 150 cancelled")
# Book state: bids={}, asks={}

print("\n" + "="*60)
print(" SCENARIO 6 — Cancel a resting order")
print("="*60)
send_order(order_conn, "35=D|11=ORD007|54=1|44=148.00|38=500",
           "LIMIT BUY  500 @ $148.00  →  expect ACK (39=0), rests in book")

send_order(order_conn, "35=F|11=ORD007",
           "CANCEL ORD007             →  expect CANCEL (39=4)")
# Book state: bids={148.00:[ORD007(CANCELLED tombstone)]}, asks={}

print("\n" + "="*60)
print(" SCENARIO 7 — MARKET order with empty book side")
print("="*60)
# No bids left (ORD007 was cancelled). MARKET SELL has nothing to match against.
send_order(order_conn, "35=D|11=ORD008|54=2|38=100",
           "MARKET SELL 100           →  expect CANCEL (39=4), no bids available")

time.sleep(0.2)
print("\n" + "="*60)
print(" Done.")
print("="*60)

order_conn.close()
trade_conn.close()
