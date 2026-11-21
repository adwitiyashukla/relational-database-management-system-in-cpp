#include <atomic>
#include <thread>

#include "client.h"
#include "server.h"
#include "test.h"

using namespace rdbms;

namespace {

struct Net {
  SimFs fs{61};
  std::unique_ptr<Engine> e;
  std::unique_ptr<Server> srv;

  explicit Net(int max_conn = 64) {
    EngineOptions o;
    o.dir = "db";
    o.pool_pages = 512;
    o.lock_timeout_ms = 5000;
    o.gc_interval_ms = 5;
    e = std::make_unique<Engine>(fs, o);
    e->Open();
    Config c;
    c.port = 0;
    c.max_connections = max_conn;
    srv = std::make_unique<Server>(*e, c);
    srv->Start();
  }

  ~Net() {
    srv->Stop();
    e->Close();
  }

  std::unique_ptr<PgClient> Connect() {
    auto c = std::make_unique<PgClient>();
    c->Connect("127.0.0.1", srv->port());
    return c;
  }
};

std::string Cell(const PgResult& r, size_t row = 0, size_t col = 0) {
  if (row >= r.rows.size() || !r.rows[row][col]) return "NULL";
  return *r.rows[row][col];
}

}

TEST(simple_queries_round_trip) {
  Net net;
  auto c = net.Connect();
  CHECK_EQ(c->Exec("create table t (id int primary key, v text)").tag, std::string("CREATE TABLE"));
  CHECK_EQ(c->Exec("insert into t values (1, 'one'), (2, null)").tag, std::string("INSERT 0 2"));
  PgResult r = c->Exec("select id, v from t order by id");
  CHECK_EQ(r.columns.size(), 2u);
  CHECK_EQ(r.columns[1], std::string("v"));
  CHECK_EQ(Cell(r, 0, 1), std::string("one"));
  CHECK_EQ(Cell(r, 1, 1), std::string("NULL"));
  CHECK_EQ(r.tag, std::string("SELECT 2"));
  auto many = c->Query("begin; insert into t values (3, 'x'); select count(*) from t");
  CHECK_EQ(many.size(), 3u);
  CHECK_EQ(Cell(many[2]), std::string("3"));
  CHECK(c->status() == 'T');
  CHECK_EQ(c->Exec("insert into t values (3, 'dup')").code, std::string(state::kUnique));
  CHECK(c->status() == 'E');
  CHECK_EQ(c->Exec("select 1").code, std::string(state::kFailedTxn));
  CHECK_EQ(c->Exec("commit").tag, std::string("ROLLBACK"));
  CHECK(c->status() == 'I');
  CHECK_EQ(Cell(c->Exec("select count(*) from t")), std::string("2"));
  CHECK_EQ(c->Query("").size(), 1u);
  CHECK_EQ(c->Exec("select from where").code, std::string(state::kSyntax));
}

TEST(extended_protocol_binds_parameters) {
  Net net;
  auto c = net.Connect();
  c->Exec("create table p (id bigint primary key, name text, score float8)");
  CHECK_EQ(c->Prepared("insert into p values ($1, $2, $3)", {"7", "ada", "1.5"}).tag,
           std::string("INSERT 0 1"));
  CHECK_EQ(c->Prepared("insert into p values ($1, $2, $3)", {"8", std::nullopt, "2"}).tag,
           std::string("INSERT 0 1"));
  PgResult r = c->Prepared("select name, score * 2 from p where id = $1", {"7"});
  CHECK_EQ(Cell(r, 0, 0), std::string("ada"));
  CHECK_EQ(Cell(r, 0, 1), std::string("3"));
  CHECK_EQ(Cell(c->Prepared("select count(*) from p where name is null and id > $1", {"0"})),
           std::string("1"));
  CHECK_EQ(c->Prepared("select $1 + 1", {"41"}).rows.size(), 1u);
  CHECK_EQ(c->Prepared("select * from p where id = $1", {"1", "2"}).code,
           std::string(state::kProtocol));
  CHECK_EQ(Cell(c->Exec("select 1")), std::string("1"));
}

TEST(concurrent_clients_keep_money_balanced) {
  Net net;
  {
    auto c = net.Connect();
    c->Exec("create table acct (id int primary key, bal bigint)");
    std::string sql = "insert into acct values ";
    for (int i = 0; i < 30; i++)
      sql += (i ? ", " : "") + std::string("(") + std::to_string(i) + ", 1000)";
    c->Exec(sql);
  }
  std::atomic<int> done{0}, retried{0}, unexpected{0};
  std::vector<std::thread> threads;
  for (int w = 0; w < 8; w++) {
    threads.emplace_back([&, w] {
      PgClient c;
      c.Connect("127.0.0.1", net.srv->port());
      const char* iso[] = {"read committed", "repeatable read", "serializable"};
      Rng rng(static_cast<uint64_t>(w) + 9);
      for (int i = 0; i < 150; i++) {
        int a = static_cast<int>(rng.Uniform(30)), b = static_cast<int>(rng.Uniform(30));
        if (a == b) continue;
        for (int attempt = 0; attempt < 50; attempt++) {
          auto rs = c.Query(std::string("begin isolation level ") + iso[w % 3] +
                            "; update acct set bal = bal - 3 where id = " + std::to_string(a) +
                            "; update acct set bal = bal + 3 where id = " + std::to_string(b) +
                            "; commit");
          bool ok = true;
          for (auto& r : rs) {
            if (r.ok()) continue;
            ok = false;
            if (r.code != state::kSerialization && r.code != state::kDeadlock) unexpected++;
          }
          if (c.status() != 'I') c.Exec("rollback");
          if (ok) {
            done++;
            break;
          }
          retried++;
        }
      }
    });
  }
  for (auto& t : threads) t.join();
  CHECK_EQ(unexpected.load(), 0);
  auto c = net.Connect();
  CHECK_EQ(Cell(c->Exec("select sum(bal), count(*) from acct")), std::string("30000"));
  CHECK(done.load() > 1000);
}

TEST(cancel_request_stops_a_lock_wait) {
  Net net;
  auto a = net.Connect();
  auto b = net.Connect();
  a->Exec("create table q (id int primary key, v int)");
  a->Exec("insert into q values (1, 0)");
  a->Query("begin; update q set v = 1 where id = 1");
  std::string code;
  std::thread waiter([&] { code = b->Exec("update q set v = 2 where id = 1").code; });
  SleepMicros(150000);
  PgCancel("127.0.0.1", net.srv->port(), b->pid(), b->secret());
  waiter.join();
  CHECK_EQ(code, std::string(state::kCanceled));
  a->Exec("commit");
  CHECK_EQ(Cell(b->Exec("select v from q")), std::string("1"));
}

TEST(disconnecting_rolls_back_an_open_transaction) {
  Net net;
  auto a = net.Connect();
  a->Exec("create table r (id int primary key)");
  a->Query("begin; insert into r values (1)");
  a->Close();
  SleepMicros(100000);
  auto b = net.Connect();
  CHECK_EQ(Cell(b->Exec("select count(*) from r")), std::string("0"));
  CHECK_EQ(b->Exec("insert into r values (1)").tag, std::string("INSERT 0 1"));
}

TEST(connection_limit_is_enforced) {
  Net net(2);
  auto a = net.Connect();
  auto b = net.Connect();
  bool refused = false;
  try {
    net.Connect();
  } catch (const DbError& e) {
    refused = e.code() == state::kTooMany;
  }
  CHECK(refused);
}
