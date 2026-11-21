# relational-database-management-system-in-cpp

A relational database management system I wrote from scratch in C++20. It runs as a server that speaks the PostgreSQL wire protocol, so `psql`, `pgbench` and drivers like psycopg connect to it as if it were Postgres. Underneath there is MVCC with three isolation levels (read committed, repeatable read as snapshot isolation, and serializable through serializable snapshot isolation), an ARIES-style write-ahead log with crash recovery, a B+tree over 8 KB pages, and a buffer pool with a doublewrite buffer. The short name used for the binaries and the C++ namespace is `rdbms`.

I built it to learn what actually happens between `BEGIN` and `COMMIT`. Most of the time went into trying to break it: a crash simulator that drops and tears unsynced writes, an isolation checker that hunts for anomalies in real concurrent histories, and force-killing the server over and over in the middle of pgbench runs. Two of those found real bugs, which are written up below. The last section compares it with PostgreSQL 16 on my laptop.

## Features

- PostgreSQL v3 wire protocol: the simple and extended query flows (Parse, Bind, Describe, Execute, Sync), cancel requests and real SQLSTATE codes, so client retry logic for `40001` and `40P01` works unchanged.
- SQL: `CREATE TABLE` and `DROP TABLE` with a primary key, multi-row `INSERT`, `UPDATE`, `DELETE`, and single-table `SELECT` with `WHERE`, `GROUP BY`, `HAVING`, `ORDER BY`, `LIMIT`, `OFFSET`, `DISTINCT`, aggregates, `CASE`, `CAST`, `LIKE`, `IN`, `BETWEEN` and `FOR UPDATE`. Also `BEGIN`, `COMMIT`, `ROLLBACK`, `SET TRANSACTION`, `SHOW`, `EXPLAIN` and `CHECKPOINT`. There are no joins.
- Types: boolean, smallint, int, bigint, real, double precision, numeric, text, varchar, char and timestamp.
- MVCC, so readers never wait for writers. The newest version of each row lives in the B+tree leaf, and older versions sit in an in-memory version store until no snapshot can see them.
- Three isolation levels: read committed, repeatable read (snapshot isolation with first-updater-wins) and serializable (SSI, the same approach PostgreSQL uses).
- Row-level write locks with FIFO wait queues, deadlock detection on a waits-for graph, and a lock timeout.
- Write-ahead logging with a CRC32C on every record, LSNs that are byte offsets into 16 MB segments, leader-based group commit and fuzzy checkpoints.
- ARIES recovery (analysis, redo, undo) with compensation log records, so a crash during undo is safe too.
- Checksummed pages, a buffer pool with clock eviction, and a doublewrite buffer, so a page torn by a crash gets repaired at startup.
- All tunables in `config.toml`.

## Quick start

You need CMake 3.16 or newer and a C++20 compiler (GCC, Clang or MSVC 2022).

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Start the server from the repo root so it picks up `config.toml`. It keeps its files in `data/`. On Linux and macOS:

```
./build/rdbms-server
```

On Windows, where the Visual Studio generator puts binaries under `Release`:

```
build\Release\rdbms-server.exe
```

Then connect with any Postgres client. There is no authentication: the server listens on 127.0.0.1 and accepts any user name. It uses port 5433 so it can run next to a real Postgres on 5432.

```
psql -h 127.0.0.1 -p 5433 -U me -d rdbms
```

```sql
create table accounts (id int primary key, owner text not null, balance bigint not null);
insert into accounts values (1, 'ana', 100), (2, 'ben', 100), (3, 'cy', 50);
begin isolation level serializable;
update accounts set balance = balance - 30 where id = 1;
update accounts set balance = balance + 30 where id = 2;
commit;
select owner, balance from accounts order by id;
explain select * from accounts where id = 2;
```

## Configuration

`config.toml` in the repo lists every setting with its default. Use `--config path` to load a different file.

| Setting | Default | What it controls |
|---|---|---|
| `server.host` | `127.0.0.1` | Address to listen on |
| `server.port` | `5433` | Port |
| `server.max_connections` | `64` | Connections allowed at once |
| `storage.data_dir` | `data` | Folder for the data file, the doublewrite file and the WAL |
| `storage.buffer_pool_mb` | `64` | Buffer pool size |
| `wal.segment_mb` | `16` | WAL segment size |
| `wal.fsync` | `true` | Sync the WAL before a commit is acknowledged |
| `wal.group_commit_delay_us` | `0` | How long a commit leader waits for followers before it flushes |
| `checkpoint.interval_s` | `30` | Time between checkpoints |
| `checkpoint.wal_mb` | `64` | Checkpoint early once this much WAL is written |
| `txn.default_isolation` | `read committed` | Level for transactions that don't pick one |
| `txn.lock_timeout_ms` | `10000` | How long to wait for a row lock before giving up |
| `gc.interval_ms` | `20` | How often old versions and SSI state get cleaned up |

## How it works

### Storage

Everything lives in one data file of 8 KB checksummed pages. Each table, and the catalog itself, is a B+tree on the primary key with slotted leaf pages. The buffer pool evicts with the clock algorithm. Dirty pages never go straight to the data file: they're first written in batches of up to 64 to a doublewrite file and synced, so if a crash tears a page halfway through, startup copies the intact image back from the doublewrite file. Only one thread flushes a given page at a time, so an older copy can never land on disk after a newer one. If the page changes while it's being written, it stays dirty, and its recovery LSN moves up to the LSN of the copy that reached disk, which is always the start of a log record.

### Write-ahead log and recovery

Every change is logged before its page can be written. A record's LSN is its byte offset in the log, each record carries a CRC32C, and the log is split into 16 MB segment files. Commits use leader-based group commit: the first transaction to reach the flush becomes the leader, writes everything buffered so far, and syncs once (`fdatasync` on Linux, `F_FULLFSYNC` on macOS, `FlushFileBuffers` on Windows). Every transaction whose commit record made it into that write returns together. If a sync ever fails, the log refuses all further work instead of retrying, because after a failed fsync you can't trust what reached the disk.

A background writer keeps the number of dirty pages down and takes fuzzy checkpoints every 30 seconds or 64 MB of WAL. A checkpoint records the dirty page table and the active transaction table without stopping anyone, and then the log before the oldest LSN that is still needed gets truncated. Recovery is ARIES. Analysis rebuilds both tables from the last checkpoint, redo repeats history from the oldest dirty page's LSN and uses page LSNs to skip work that already reached disk, and undo rolls back the losers while writing compensation log records, so a crash in the middle of undo just picks up where it left off.

### MVCC and isolation

Each transaction gets an id when it starts and a commit timestamp once its commit record is on disk, which is also the moment its writes become visible. A snapshot is just the commit clock at the time it's taken. When a transaction overwrites a row, the previous version moves into the version store, keyed by table and primary key. That store doesn't need to survive a crash, since no snapshot is older than the restart. GC drops old versions once every active snapshot can see their successor, and purges deleted rows from the tree once nobody can see them.

- Read committed takes a new snapshot for each statement. An update that finds a row locked waits for the lock and then works on the latest committed version, like Postgres.
- Repeatable read keeps one snapshot for the whole transaction. If it tries to update a row that someone else committed after that snapshot, it fails with `40001`.
- Serializable is snapshot isolation plus SSI, described next.

Writers take an exclusive lock on each row they change and hold it until commit or abort. Waiters queue in FIFO order. Every time a transaction starts waiting, the waits-for graph is checked for a cycle, and the transaction that would close it gets `40P01`. A wait that runs past `lock_timeout_ms` fails with `55P03`.

### Serializable snapshot isolation

Snapshot isolation only goes wrong through rw-antidependencies, where T1 reads something and a concurrent T2 writes a newer version that T1 didn't see. Every non-serializable cycle under snapshot isolation contains a pivot: a transaction with an incoming and an outgoing rw-antidependency, where the transaction on the outgoing side committed first. This result is from Fekete et al. (2005) and is the basis for SSI (Cahill et al., 2008) and for PostgreSQL's implementation (Ports and Grittner, 2012). The server tracks these edges and aborts a transaction whenever such a structure could form.

Edges come from two places. Serializable reads leave SIREAD locks on the keys they read and on the key ranges they scan, so phantoms count too. A later write by a concurrent transaction to a locked key or range creates an edge. And when a serializable read has to skip a version that is too new for its snapshot, that creates an edge right away. To keep memory bounded, a transaction that collects more than 10,000 SIREAD locks on one table gets one lock on the whole table instead.

The checks run whenever an edge is added and again at commit, and they follow commit order, so only a structure whose outgoing transaction commits first counts. If the transaction that would complete the structure is the one currently running, it gets `40001` right away. If the pivot is some other transaction that hasn't committed yet, it gets marked doomed and fails at its next statement or at commit. State for a committed transaction stays around until no running transaction overlaps it. When GC drops one, each transaction with an edge into it remembers the earliest commit among the dropped neighbors, the same idea as PostgreSQL's conflict-out summary. That last part came from a bug the isolation checker found, described below.

### Wire protocol

Each connection gets its own thread. The server handles the startup handshake (SSL requests are declined, so clients fall back to plain TCP), the simple query flow, the extended query flow with named and unnamed statements and portals, the `ParameterStatus` messages that drivers look for, cancel requests over a second connection, and error responses with SQLSTATE codes.

## Testing

- Unit tests for every layer in `tests/` (storage, WAL, B+tree, transactions, recovery, SQL and the server), run by `ctest`.
- A crash simulator, `rdbms-crashsim`. It runs the engine on a simulated file system and crashes it at a random point. Every write since the last sync is then independently kept, dropped, or torn at 512-byte sector boundaries, and files whose directory was never synced disappear. Then it recovers and compares every row with a model of what had committed. A commit that was in flight at the crash may land either way, but nothing else may change. CI runs 1,000 seeds, and I ran 2,000 seeds on both Linux and Windows with zero failures.
- An isolation checker, `tools/isolation_check.py`, in the style of Jepsen's Elle. Ten clients run random transactions that append unique numbers to lists stored in rows and read lists back. From the reads it rebuilds each list's version order and the write-write, write-read and read-write dependencies between committed transactions, then looks for cycles and classifies them as G0, G1a, G1b, G1c, G-single or G2. Read committed may show G-single and G2, repeatable read may show G2 (write skew), and serializable must show nothing.
- GitHub Actions CI. It builds and tests with GCC and Clang on Ubuntu, Clang on macOS and MSVC on Windows. It also runs ASan, UBSan and TSan builds and a clang-format check. Finally, an integration job runs the crash sweep, the isolation checker and pgbench. Then it does `kill -9` in the middle of pgbench six times, with an 8 MB buffer pool and a checkpoint every second, and after each restart it checks that the TPC-B balances still add up.

### Two bugs the tests caught

The isolation checker found the first one. Right before publishing, a CI dry run reported a single G2 cycle at serializable. The cycle had three transactions. T2 read a row, and then T3 overwrote that row and committed. T1 started after that and saw T3's write. Then T2 overwrote a row that T1 read without seeing it. T2 is a pivot whose outgoing neighbor committed first, so someone had to be aborted. The server did record the edge from T2 to T3, but once T2 had committed, GC was allowed to drop T3, because no running transaction overlapped T3 anymore. When T1 later read past T2's version, the new edge into T2 had nothing behind it. The fix is the conflict-out summary described above. The regression test `committed_pivot_keeps_its_conflict_after_gc` replays that exact interleaving. With GC running every millisecond to make the race common, the old code produced about 15 G2 cycles in every 20-second run, and the fixed code produced none.

The second one showed up while benchmarking. After I force-killed the server at the end of a run with fsync turned off, it refused to start again with `page 0 is not a tree node`. A loop that kills and restarts the server under pgbench, with a small buffer pool and frequent checkpoints, reproduced it within one or two rounds, and once the TPC-B balances also came back out of sync. The cause was in the buffer pool. When a page changed while it was being flushed, the old code set its recovery LSN to the flushed copy's LSN plus one, which isn't the start of any log record. When that became the oldest entry in the checkpoint's dirty page table, redo started at that byte, couldn't read a record there, and quietly skipped the whole redo pass. While reading that code I also found that two threads could flush the same page at once, so an older copy could land on disk after a newer one. Both are fixed, and redo now fails loudly if it can't read the log. The regression test `rows_changed_during_a_flush_are_redone` uses a small test file system to change a row in the middle of a flush, and it fails on the old code. After the fix, the same loop passed 52 kill-and-restart rounds across three configurations. The old CI kill test missed it because with a 64 MB pool and a checkpoint every 30 seconds, pages were rarely flushed while being changed, which is why it now uses a tiny buffer pool and one-second checkpoints.

## Benchmarks against PostgreSQL 16

Everything here ran on my laptop: an HP OmniBook 5 with an Intel Core 5 120U, 8 GB of RAM and a Samsung 512 GB NVMe SSD, on Windows 11 Home and plugged in. PostgreSQL 16.15 ran as the Windows service from the standard installer, with fsync on, synchronous_commit on, wal_sync_method open_datasync (the Windows default) and 160 MB of shared_buffers. rdbms used the repo's `config.toml` with a 128 MB buffer pool. Both servers ran on the same machine, and pgbench 16 drove both of them over localhost with the same scripts, alternating between the two at every point so they saw about the same conditions. Every number is a single run. On a laptop, single runs can move by 10 to 25 percent, especially with one client, so only the big gaps mean anything.

### TPC-B

This uses pgbench's scale 10 data (1,000,000 accounts, 100 tellers, 10 branches) and the standard TPC-B-like transaction, wrapped in `BEGIN ISOLATION LEVEL ...` so both servers run it at the same level. Each point ran for 20 seconds with `--max-tries=0`, so serialization failures and deadlocks get retried until they succeed. rdbms gets its copy of the data from `rdbms-bench load --scale 10`, which builds the same four tables. Retried is the share of transactions that needed at least one retry.

| Isolation | Clients | PostgreSQL 16 tps | rdbms tps | Postgres / rdbms | Retried, Postgres | Retried, rdbms |
|---|---|---|---|---|---|---|
| read committed | 1 | 802 | 298 | 2.7x | 0.0% | 0.0% |
| read committed | 4 | 3,069 | 651 | 4.7x | 0.0% | 0.0% |
| read committed | 8 | 4,862 | 1,115 | 4.4x | 0.0% | 0.0% |
| read committed | 16 | 6,187 | 1,410 | 4.4x | 0.0% | 0.0% |
| repeatable read | 1 | 718 | 270 | 2.7x | 0.0% | 0.0% |
| repeatable read | 4 | 2,244 | 632 | 3.6x | 21.6% | 20.4% |
| repeatable read | 8 | 3,265 | 1,016 | 3.2x | 37.6% | 32.5% |
| repeatable read | 16 | 3,391 | 1,320 | 2.6x | 51.3% | 49.3% |
| serializable | 1 | 542 | 310 | 1.8x | 0.0% | 0.0% |
| serializable | 4 | 2,490 | 691 | 3.6x | 22.0% | 20.6% |
| serializable | 8 | 3,227 | 848 | 3.8x | 37.6% | 34.6% |
| serializable | 16 | 3,138 | 973 | 3.2x | 50.8% | 50.9% |

Postgres is 1.8 to 4.7 times faster here, and most of that comes from the commit path, covered below. The retry rates at repeatable read and serializable stay within about five points of each other, so rdbms isn't aborting more often than Postgres. After the sweep, the account, teller, branch and history sums matched on both servers.

### Hot rows

A 10-row table where each transaction moves one unit between two random rows, updating them in key order so lock waits can't deadlock. 8 clients, 15 seconds each. The total stayed at 10,000,000 on both servers.

| Isolation | PostgreSQL 16 tps | rdbms tps | Retried, Postgres | Retried, rdbms |
|---|---|---|---|---|
| read committed | 3,850 | 543 | 0.0% | 0.0% |
| repeatable read | 2,400 | 550 | 52.6% | 54.8% |
| serializable | 2,427 | 582 | 53.1% | 58.5% |

Postgres is 4 to 7 times faster on this one, and rdbms retries a little more at serializable, 58.5 percent against 53.1.

### Commit path

One single-row `INSERT` per transaction in autocommit mode, 10 seconds per point. This mostly measures how fast each server can make a commit durable, and how well it shares that cost between clients.

| Clients | PostgreSQL 16 tps | Postgres latency | rdbms tps | rdbms latency |
|---|---|---|---|---|
| 1 | 5,891 | 0.17 ms | 469 | 2.13 ms |
| 4 | 13,939 | 0.29 ms | 926 | 4.32 ms |
| 16 | 30,947 | 0.52 ms | 3,732 | 4.29 ms |
| 32 | 50,203 | 0.64 ms | 6,008 | 5.33 ms |

Group commit is doing its job: going from 1 to 32 clients, rdbms gets 12.8 times the throughput while the average latency only goes from 2.13 to 5.33 ms. Postgres is still 8 to 15 times faster at every client count, which leads to the next part.

### Why Postgres is faster here

Most of the gap is the cost of making the log durable on Windows. rdbms syncs its WAL with `FlushFileBuffers`. The Postgres default on Windows, `wal_sync_method = open_datasync`, opens the WAL with `FILE_FLAG_WRITE_THROUGH` instead, so each write goes straight to the device and no separate flush call is needed. I timed both on this SSD with a small script that does 400 writes of 8 KB each:

| Method | Average | p50 | p99 |
|---|---|---|---|
| Write, then `FlushFileBuffers` | 1.19 ms | 1.18 ms | 1.73 ms |
| Write-through write | 0.20 ms | 0.17 ms | 0.66 ms |

So every group flush in rdbms costs about six times what a Postgres flush costs. To see how much of the gap that explains, I also ran rdbms once with `wal.fsync = false`. Pages still go through the synced doublewrite buffer in that mode.

| Test | rdbms, fsync on | rdbms, fsync off | PostgreSQL 16, fsync on |
|---|---|---|---|
| TPC-B read committed, 1 client | 298 | 1,466 | 802 |
| TPC-B read committed, 16 clients | 1,410 | 8,401 | 6,187 |
| Commit path, 1 client | 469 | 8,264 | 5,891 |
| Commit path, 16 clients | 3,732 | 33,368 | 30,947 |

With the WAL sync turned off, rdbms is faster than Postgres on all four of these, so the engine itself keeps up and most of the gap is the commit flush. That comparison isn't fair to Postgres, which is still syncing, so the fsync-off column is only there to show where the time goes. The Postgres single-client commit latency above, 0.17 ms, is right at the write-through number from the timing script.

### Recovery

`rdbms-bench recovery` loads a 100,000-row table, runs 40,000 transactions of four random updates each with WAL syncing off, makes sure the log is written, then stops without writing any dirty pages and times the restart. More frequent checkpoints mean less log to replay, but they slow the workload down.

| Checkpoint every | Checkpoints taken | Workload time | Log replayed | Changes redone | Restart time |
|---|---|---|---|---|---|
| never | 1 | 2.2 s | 40.8 MB | 160,000 | 1.81 s |
| 4 MB | 11 | 6.6 s | 4.8 MB | 3,402 | 0.98 s |
| 16 MB | 3 | 3.1 s | 8.8 MB | 34,560 | 1.37 s |
| 64 MB | 1 | 2.3 s | 40.8 MB | 160,000 | 1.31 s |

The checkpoint count includes the one taken right after loading. Checkpointing every 4 MB cut the log to replay from 40.8 MB to 4.8 MB and the restart from 1.81 to 0.98 seconds, but the same 40,000 transactions took 6.6 seconds instead of 2.2. The 64 MB setting never triggered during this workload, so that row did the same work as the first one, and the gap between their restart times (1.81 and 1.31 seconds) shows how much these single runs move around.

### Kill tests and the isolation checker on Windows

Three times, I force-killed rdbms 8 seconds into an 8-client pgbench TPC-B run, restarted it and checked the four TPC-B sums. Recovery took 1.31 to 1.63 seconds and rolled back 2 to 4 in-flight transactions, and the sums matched every time. Then I did five more rounds with an 8 MB buffer pool and a checkpoint every second, the setup that used to break it. Those came back consistent too, and in one of them the kill landed in the middle of a page write, so startup repaired a torn page from the doublewrite buffer. I also killed the server right after the fsync-off runs above, which is how the recovery bug first showed up, and this time it came back with every sum matching. The crash simulator ran 2,000 seeds on Windows with zero failures.

The isolation checker on Windows, 15 seconds per level:

| Isolation | Committed | Aborted | G-single | G2 |
|---|---|---|---|---|
| read committed | 11,008 | 314 | 89 | 284 |
| repeatable read | 8,082 | 5,574 | 0 | 316 |
| serializable | 7,524 | 6,760 | 0 | 0 |

Read committed and repeatable read show only the anomalies those levels allow, and serializable shows none.

## Project layout

| Path | What's there |
|---|---|
| `src/fs.*` | File system interface, the real one for each OS and the crash-simulating one |
| `src/page.*`, `src/btree.*` | Slotted pages and the B+tree |
| `src/buffer.*` | Buffer pool, clock eviction and the doublewrite buffer |
| `src/wal.*`, `src/recovery.cpp` | Log manager, group commit, checkpoints and ARIES recovery |
| `src/txn.*` | Transaction manager, lock manager, version store and SSI |
| `src/engine.*` | Ties storage, logging and transactions together |
| `src/parser.cpp`, `src/sql.h`, `src/executor.*`, `src/value.*` | SQL parsing, execution and types |
| `src/session.*`, `src/server.*`, `src/net.*` | Sessions, the wire protocol and sockets |
| `src/client.*` | Small wire protocol client used by the tools and tests |
| `src/common.*`, `src/config.*`, `src/main.cpp` | Shared helpers, `config.toml` parsing and the server's entry point |
| `tools/bench.cpp` | `rdbms-bench`: data loading, TPC-B, hot rows, commit and recovery benchmarks |
| `tools/crashsim.cpp` | `rdbms-crashsim`: randomized crash and recovery testing |
| `tools/isolation_check.py` | List-append isolation checker |
| `tests/` | Unit tests |

## License

MIT, see `LICENSE`.
