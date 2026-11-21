import argparse
import random
import sys
import threading
import time
from collections import defaultdict

import psycopg

LEVELS = ["read committed", "repeatable read", "serializable"]
RETRYABLE = ("40001", "40P01")


class Keys:
    def __init__(self, count, limit):
        self.lock = threading.Lock()
        self.active = list(range(count))
        self.next = count
        self.limit = limit
        self.appends = defaultdict(int)

    def read(self, rnd):
        with self.lock:
            return rnd.choice(self.active)

    def append(self, rnd):
        with self.lock:
            key = rnd.choice(self.active)
            self.appends[key] += 1
            if self.appends[key] >= self.limit:
                self.active[self.active.index(key)] = self.next
                self.next += 1
            return key


class Txn:
    def __init__(self, tid, client):
        self.tid = tid
        self.client = client
        self.ops = []
        self.status = "unknown"


def connect(args):
    return psycopg.connect(host=args.host, port=args.port, user="checker", dbname="rdbms", autocommit=True)


def setup(args):
    with connect(args) as conn:
        conn.execute("drop table if exists la")
        conn.execute("create table la (k int primary key, v text not null)")
        total = args.keys + 20000
        for start in range(0, total, 1000):
            values = ", ".join("(%d, '')" % k for k in range(start, min(start + 1000, total)))
            conn.execute("insert into la values " + values)


def client(args, level, cid, keys, out, lock, stop):
    rnd = random.Random(args.seed * 1000 + cid)
    conn = connect(args)
    cur = conn.cursor()
    counter = 0
    local = []
    while not stop.is_set():
        txn = Txn(None, cid)
        plan = []
        for _ in range(rnd.randint(1, args.ops)):
            if rnd.random() < 0.5:
                plan.append(("r", keys.read(rnd), None))
            else:
                counter += 1
                plan.append(("a", keys.append(rnd), cid * 10000000 + counter))
        try:
            cur.execute("begin isolation level " + level)
            for kind, key, val in plan:
                if kind == "r":
                    cur.execute("select v from la where k = %s", (key,))
                    row = cur.fetchone()
                    seen = [int(x) for x in row[0].split(",")] if row and row[0] else []
                    txn.ops.append(("r", key, seen))
                else:
                    txn.ops.append(("a", key, val))
                    cur.execute(
                        "update la set v = case when v = '' then %s else v || ',' || %s end where k = %s",
                        (str(val), str(val), key),
                    )
            cur.execute("commit")
            txn.status = "ok"
        except psycopg.Error as e:
            code = getattr(e, "sqlstate", None)
            txn.status = "fail" if code in RETRYABLE else "unknown"
            if code not in RETRYABLE:
                print("client %d: unexpected error %s: %s" % (cid, code, e), file=sys.stderr)
            try:
                cur.execute("rollback")
            except psycopg.Error:
                pass
        local.append(txn)
    conn.close()
    with lock:
        out.extend(local)


def run(args, level):
    setup(args)
    history = []
    lock = threading.Lock()
    stop = threading.Event()
    keys = Keys(args.keys, 80)
    threads = [
        threading.Thread(target=client, args=(args, level, c, keys, history, lock, stop))
        for c in range(args.clients)
    ]
    for t in threads:
        t.start()
    time.sleep(args.seconds)
    stop.set()
    for t in threads:
        t.join()
    for i, t in enumerate(history):
        t.tid = i
    return history


def find_cycle(graph, nodes):
    members = set(nodes)
    start = nodes[0]
    parent = {start: None}
    queue = [start]
    while queue:
        cur = queue.pop(0)
        for nxt, kind in graph[cur]:
            if nxt not in members:
                continue
            if nxt == start:
                path = []
                node = cur
                path.append((cur, start, kind))
                while parent[node] is not None:
                    prev, how = parent[node]
                    path.append((prev, node, how))
                    node = prev
                return list(reversed(path))
            if nxt not in parent:
                parent[nxt] = (cur, kind)
                queue.append(nxt)
    return []


def strongly_connected(graph, count):
    index = {}
    low = {}
    stack = []
    on = set()
    out = []
    counter = [0]
    for root in range(count):
        if root in index:
            continue
        work = [(root, 0)]
        while work:
            node, i = work.pop()
            if i == 0:
                index[node] = low[node] = counter[0]
                counter[0] += 1
                stack.append(node)
                on.add(node)
            edges = graph[node]
            if i < len(edges):
                work.append((node, i + 1))
                nxt = edges[i][0]
                if nxt not in index:
                    work.append((nxt, 0))
                elif nxt in on:
                    low[node] = min(low[node], index[nxt])
                continue
            for nxt, _ in edges:
                if nxt in on:
                    low[node] = min(low[node], low[nxt])
            if low[node] == index[node]:
                comp = []
                while True:
                    w = stack.pop()
                    on.discard(w)
                    comp.append(w)
                    if w == node:
                        break
                if len(comp) > 1:
                    out.append(comp)
    return out


def analyze(history):
    found = defaultdict(int)
    examples = {}
    writer = {}
    final_append = {}
    for t in history:
        last_by_key = {}
        for kind, key, val in t.ops:
            if kind == "a":
                writer[val] = t
                last_by_key[key] = val
        final_append[t.tid] = last_by_key
    committed = [t for t in history if t.status == "ok"]
    longest = {}
    for t in committed:
        mine = defaultdict(list)
        for kind, key, val in t.ops:
            if kind == "a":
                mine[key].append(val)
                continue
            if len(set(val)) != len(val):
                found["duplicate elements"] += 1
            for x in val:
                w = writer.get(x)
                if w is None:
                    found["garbage read"] += 1
                elif w.status == "fail":
                    found["G1a"] += 1
            if mine[key] and val[-len(mine[key]):] != mine[key]:
                found["internal"] += 1
            if val:
                w = writer.get(val[-1])
                if w is not None and w is not t and final_append[w.tid].get(key) != val[-1]:
                    found["G1b"] += 1
            if len(val) > len(longest.get(key, [])):
                longest[key] = val
    for t in committed:
        for kind, key, val in t.ops:
            if kind == "r" and longest.get(key, [])[: len(val)] != val:
                found["incompatible order"] += 1
    graph = defaultdict(list)
    seen = set()

    def edge(a, b, kind):
        if a is b or a.status != "ok" or b.status != "ok":
            return
        if (a.tid, b.tid, kind) in seen:
            return
        seen.add((a.tid, b.tid, kind))
        graph[a.tid].append((b.tid, kind))

    for key, order in longest.items():
        for x, y in zip(order, order[1:]):
            edge(writer[x], writer[y], "ww")
    for t in committed:
        for kind, key, val in t.ops:
            if kind != "r":
                continue
            order = longest.get(key, [])
            if val:
                edge(writer[val[-1]], t, "wr")
            if len(val) < len(order):
                edge(t, writer[order[len(val)]], "rw")
    for comp in strongly_connected(graph, len(history)):
        cycle = find_cycle(graph, comp)
        kinds = [k for _, _, k in cycle]
        rw = kinds.count("rw")
        if not kinds:
            name = "cycle"
        elif rw == 0 and "wr" not in kinds:
            name = "G0"
        elif rw == 0:
            name = "G1c"
        elif rw == 1:
            name = "G-single"
        else:
            name = "G2"
        found[name] += 1
        examples.setdefault(name, " -> ".join("T%d" % a for a, _, _ in cycle) + " (" + ", ".join(kinds) + ")")
    return committed, found, examples


def main():
    p = argparse.ArgumentParser(description="list-append isolation checker in the style of Elle")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=5433)
    p.add_argument("--clients", type=int, default=10)
    p.add_argument("--keys", type=int, default=8)
    p.add_argument("--ops", type=int, default=4)
    p.add_argument("--seconds", type=float, default=20)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--isolation", default="all", choices=LEVELS + ["all"])
    args = p.parse_args()
    levels = LEVELS if args.isolation == "all" else [args.isolation]
    kinds = ["G0", "G1a", "G1b", "G1c", "G-single", "G2"]
    print("%-16s %9s %9s  %s" % ("isolation", "committed", "aborted", "  ".join("%8s" % k for k in kinds)))
    bad = False
    for level in levels:
        history = run(args, level)
        committed, found, examples = analyze(history)
        aborted = sum(1 for t in history if t.status == "fail")
        print("%-16s %9d %9d  %s" % (level, len(committed), aborted, "  ".join("%8d" % found[k] for k in kinds)))
        for name, text in sorted(examples.items()):
            print("    example %s: %s" % (name, text))
        other = {k: v for k, v in found.items() if k not in kinds}
        if other:
            print("    other problems: %s" % other)
            bad = True
        if any(found[k] for k in ["G0", "G1a", "G1b", "G1c"]):
            bad = True
        if level == "repeatable read" and found["G-single"]:
            bad = True
        if level == "serializable" and any(found[k] for k in kinds):
            bad = True
        if any(t.status == "unknown" for t in history):
            bad = True
    print("result: %s" % ("anomalies outside what each level allows" if bad else "every level stayed within its guarantees"))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
