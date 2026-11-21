#include "session.h"
#include "test.h"

using namespace rdbms;

namespace {

struct Sql {
  SimFs fs{51};
  std::unique_ptr<Engine> e;
  std::unique_ptr<Session> s;

  Sql() {
    EngineOptions o;
    o.dir = "db";
    o.pool_pages = 256;
    o.background = false;
    o.lock_timeout_ms = 1000;
    e = std::make_unique<Engine>(fs, o);
    e->Open();
    s = std::make_unique<Session>(*e, Isolation::ReadCommitted);
  }

  std::unique_ptr<Session> Other() {
    return std::make_unique<Session>(*e, Isolation::ReadCommitted);
  }

  QueryResult Run(const std::string& sql, Session* who = nullptr) {
    Session* x = who ? who : s.get();
    QueryResult last;
    for (auto& st : ParseSql(sql)) {
      try {
        last = x->Execute(st, {});
      } catch (...) {
        x->OnError();
        throw;
      }
    }
    return last;
  }

  std::vector<std::string> Rows(const std::string& sql, Session* who = nullptr) {
    QueryResult q = Run(sql, who);
    std::vector<std::string> out;
    for (auto& row : q.rows) {
      std::string line;
      for (size_t i = 0; i < row.size(); i++)
        line += (i ? "|" : "") + (row[i].null() ? std::string("NULL") : ToText(row[i]));
      out.push_back(line);
    }
    return out;
  }

  std::string One(const std::string& sql, Session* who = nullptr) {
    auto r = Rows(sql, who);
    if (r.size() != 1) throw testing::Failure("expected one row from: " + sql);
    return r[0];
  }
};

using V = std::vector<std::string>;

}

TEST(basic_statements_and_command_tags) {
  Sql db;
  CHECK_EQ(
      db.Run("create table t (id int primary key, name text not null, score double precision)").tag,
      std::string("CREATE TABLE"));
  CHECK_EQ(db.Run("insert into t values (1, 'ann', 9.5), (2, 'bob', 7), (3, 'cy', null)").tag,
           std::string("INSERT 0 3"));
  CHECK(db.Rows("select * from t order by id") == V({"1|ann|9.5", "2|bob|7", "3|cy|NULL"}));
  CHECK_EQ(db.Run("update t set score = score + 1 where score is not null").tag,
           std::string("UPDATE 2"));
  CHECK_EQ(db.Run("delete from t where name = 'cy'").tag, std::string("DELETE 1"));
  CHECK(db.Rows("select name, score from t order by score desc") == V({"ann|10.5", "bob|8"}));
  CHECK_EQ(db.Run("insert into t (name, id) values ('dee', 4)").tag, std::string("INSERT 0 1"));
  CHECK_EQ(db.One("select score is null from t where id = 4"), std::string("t"));
  CHECK_EQ(db.Run("drop table t").tag, std::string("DROP TABLE"));
  CHECK_CODE(db.Run("select * from t"), state::kNoTable);
}

TEST(filters_ordering_limits_and_distinct) {
  Sql db;
  db.Run("create table n (k bigint primary key, g int, s text)");
  std::string sql = "insert into n values ";
  for (int i = 1; i <= 100; i++)
    sql += (i > 1 ? ", " : "") + std::string("(") + std::to_string(i) + ", " +
           std::to_string(i % 7) + ", 'x" + std::to_string(i % 3) + "')";
  db.Run(sql);
  CHECK_EQ(db.One("select count(*) from n where k between 10 and 19"), std::string("10"));
  CHECK(db.Rows("select k from n where k > 95 order by k desc limit 3") == V({"100", "99", "98"}));
  CHECK(db.Rows("select k from n order by g, k limit 2 offset 1") == V({"14", "21"}));
  CHECK(db.Rows("select distinct s from n order by s") == V({"x0", "x1", "x2"}));
  CHECK(db.Rows("select k from n where k in (5, 3, 300, 5) order by 1") == V({"3", "5"}));
  CHECK_EQ(db.One("select count(*) from n where s like 'x_' and g <> 0"), std::string("86"));
  CHECK_EQ(db.One("select count(*) from n where s ilike 'X1'"), std::string("34"));
  CHECK(db.Rows("select k from n where k < 3 or k = 100 order by k") == V({"1", "2", "100"}));
  CHECK(db.Rows("select k as kk from n where k <= 2 order by kk desc") == V({"2", "1"}));
}

TEST(aggregates_and_grouping) {
  Sql db;
  db.Run("create table s (id int primary key, team text, pts int)");
  db.Run(
      "insert into s values (1, 'a', 10), (2, 'a', 20), (3, 'b', 5), (4, 'b', null), (5, 'c', 7)");
  CHECK_EQ(db.One("select count(*), count(pts), sum(pts), min(pts), max(pts) from s"),
           std::string("5|4|42|5|20"));
  CHECK_EQ(db.One("select avg(pts) from s"), std::string("10.5"));
  CHECK(db.Rows("select team, sum(pts) from s group by team order by team") ==
        V({"a|30", "b|5", "c|7"}));
  CHECK(db.Rows("select team, count(*) from s group by 1 having count(*) > 1 order by 1") ==
        V({"a|2", "b|2"}));
  CHECK(db.Rows(
            "select pts % 5, count(*) from s where pts is not null group by pts % 5 order by 1") ==
        V({"0|3", "2|1"}));
  CHECK_EQ(db.One("select count(distinct team) from s"), std::string("3"));
  CHECK_EQ(db.One("select sum(pts) from s where id > 100"), std::string("NULL"));
  CHECK_EQ(db.One("select count(*) from s where id > 100"), std::string("0"));
  CHECK_CODE(db.Run("select team, pts from s group by team"), state::kGrouping);
  CHECK_CODE(db.Run("select * from s where count(*) > 1"), state::kGrouping);
}

TEST(expressions_follow_sql_rules) {
  Sql db;
  CHECK_EQ(db.One("select 2 + 3 * 4, (2 + 3) * 4, 7 / 2, 7 % 3, -7 / 2"),
           std::string("14|20|3|1|-3"));
  CHECK_EQ(db.One("select 7.0 / 2, 1e3, 'a' || 'b' || 1"), std::string("3.5|1000|ab1"));
  CHECK_EQ(db.One("select null = null, null is null, 1 in (2, null), 1 in (1, null)"),
           std::string("NULL|t|NULL|t"));
  CHECK_EQ(db.One("select true and null, false and null, true or null"), std::string("NULL|f|t"));
  CHECK_EQ(db.One("select case when 1 > 2 then 'x' when 2 > 1 then 'y' else 'z' end"),
           std::string("y"));
  CHECK_EQ(db.One("select case 3 when 1 then 'one' when 3 then 'three' end"), std::string("three"));
  CHECK_EQ(
      db.One("select coalesce(null, null, 4), nullif(5, 5), greatest(3, 9, 1), least(3, 9, 1)"),
      std::string("4|NULL|9|1"));
  CHECK_EQ(db.One("select '42'::int + 1, cast(2.6 as int), 5::text || 'x', '1.5'::float * 2"),
           std::string("43|3|5x|3"));
  CHECK_EQ(db.One("select length('h\xc3\xa9llo'), upper('ab'), substr('hello', 2, 3), abs(-4), "
                  "round(2.345, 2)"),
           std::string("5|AB|ell|4|2.35"));
  CHECK_EQ(db.One("select 5 between 1 and 10, 'abc' like 'a%c', 'abc' not like 'b%'"),
           std::string("t|t|t"));
  CHECK_EQ(db.One("select timestamp '2026-10-04 08:30:00' < now(), '2026-01-02'::timestamp"),
           std::string("t|2026-01-02 00:00:00"));
  CHECK_CODE(db.Run("select 1 / 0"), state::kDivZero);
  CHECK_CODE(db.Run("select 9223372036854775807 + 1"), state::kOutOfRange);
  CHECK_CODE(db.Run("select 'abc'::int"), state::kBadText);
  CHECK_CODE(db.Run("select nosuch(1)"), state::kUndefinedFunc);
}

TEST(column_types_and_constraints_are_enforced) {
  Sql db;
  db.Run(
      "create table c (id smallint primary key, code varchar(3), n int not null, ok boolean, at "
      "timestamp)");
  db.Run("insert into c values (1, 'abc', 5, 'yes', '2026-10-04 10:00:00.25')");
  CHECK_EQ(db.One("select * from c"), std::string("1|abc|5|t|2026-10-04 10:00:00.25"));
  CHECK_CODE(db.Run("insert into c values (2, 'abcd', 1, true, null)"), state::kTooLong);
  CHECK_CODE(db.Run("insert into c values (3, 'a', null, true, null)"), state::kNotNull);
  CHECK_CODE(db.Run("insert into c values (40000, 'a', 1, true, null)"), state::kOutOfRange);
  CHECK_CODE(db.Run("insert into c values (4, 'a', 1, 'maybe', null)"), state::kBadText);
  CHECK_CODE(db.Run("insert into c values (1, 'z', 1, false, null)"), state::kUnique);
  CHECK_CODE(db.Run("insert into c (id, nope) values (5, 1)"), state::kNoColumn);
  CHECK_CODE(db.Run("create table c (x int)"), state::kDupTable);
  CHECK_EQ(db.Run("create table if not exists c (x int)").tag, std::string("CREATE TABLE"));
}

TEST(primary_key_drives_the_access_path) {
  Sql db;
  db.Run("create table o (w int, d int, n int, total int, primary key (w, d, n))");
  std::string sql = "insert into o values ";
  for (int w = 1; w <= 3; w++)
    for (int d = 1; d <= 4; d++)
      for (int n = 1; n <= 5; n++)
        sql += std::string(sql.back() == ' ' ? "" : ", ") + "(" + std::to_string(w) + ", " +
               std::to_string(d) + ", " + std::to_string(n) + ", " +
               std::to_string(w * 100 + d * 10 + n) + ")";
  db.Run(sql);
  auto plan = [&](const std::string& q) { return db.Rows("explain " + q)[0]; };
  CHECK_EQ(plan("select * from o where w = 2 and d = 3 and n = 4"),
           std::string("Index Scan using o_pkey on o"));
  CHECK_EQ(plan("select * from o where w = 2 and d = 3"),
           std::string("Index Range Scan using o_pkey on o"));
  CHECK_EQ(plan("select * from o where w = 2 and d between 2 and 3"),
           std::string("Index Range Scan using o_pkey on o"));
  CHECK_EQ(plan("select * from o where d = 3"), std::string("Seq Scan on o"));
  CHECK_EQ(db.One("select total from o where w = 2 and d = 3 and n = 4"), std::string("234"));
  CHECK_EQ(db.One("select count(*) from o where w = 2 and d = 3"), std::string("5"));
  CHECK_EQ(db.One("select sum(total) from o where w = 3 and d between 2 and 3 and n > 3"),
           std::string("1318"));
  CHECK_EQ(db.One("select count(*) from o where w = 1 and d > 2"), std::string("10"));
  CHECK_EQ(db.One("select count(*) from o where d = 3"), std::string("15"));
}

TEST(updates_can_move_a_primary_key) {
  Sql db;
  db.Run("create table m (id int primary key, v text)");
  db.Run("insert into m values (1, 'a'), (2, 'b')");
  CHECK_EQ(db.Run("update m set id = id + 10 where id = 1").tag, std::string("UPDATE 1"));
  CHECK(db.Rows("select * from m order by id") == V({"2|b", "11|a"}));
  CHECK_CODE(db.Run("update m set id = 11 where id = 2"), state::kUnique);
  CHECK(db.Rows("select * from m order by id") == V({"2|b", "11|a"}));
}

TEST(transaction_blocks_behave_like_postgres) {
  Sql db;
  db.Run("create table b (id int primary key)");
  db.Run("begin");
  db.Run("insert into b values (1)");
  CHECK_CODE(db.Run("insert into b values (1)"), state::kUnique);
  CHECK(db.s->Status() == 'E');
  CHECK_CODE(db.Run("select 1"), state::kFailedTxn);
  CHECK_EQ(db.Run("commit").tag, std::string("ROLLBACK"));
  CHECK(db.s->Status() == 'I');
  CHECK_EQ(db.One("select count(*) from b"), std::string("0"));
  db.Run("begin isolation level repeatable read");
  CHECK_EQ(db.One("show transaction_isolation"), std::string("repeatable read"));
  db.Run("select 1");
  CHECK_CODE(db.Run("set transaction isolation level serializable"), state::kActiveTxn);
  db.Run("rollback");
  db.Run("set session characteristics as transaction isolation level serializable");
  CHECK_EQ(db.One("show transaction_isolation"), std::string("serializable"));
  db.Run("set default_transaction_isolation = 'read committed'");
  CHECK_EQ(db.One("show default_transaction_isolation"), std::string("read committed"));
  db.Run("begin read only");
  CHECK_CODE(db.Run("insert into b values (5)"), state::kReadOnly);
  db.Run("rollback");
  db.Run("begin");
  CHECK_CODE(db.Run("create table z (x int)"), state::kActiveTxn);
  db.Run("rollback");
  CHECK_EQ(db.Run("commit").notices.size(), 1u);
}

TEST(sessions_are_isolated_from_each_other) {
  Sql db;
  db.Run("create table acct (id int primary key, bal int)");
  db.Run("insert into acct values (1, 100), (2, 100)");
  auto other = db.Other();
  db.Run("begin isolation level repeatable read");
  CHECK_EQ(db.One("select bal from acct where id = 1"), std::string("100"));
  db.Run("update acct set bal = bal - 30 where id = 1", other.get());
  CHECK_EQ(db.One("select bal from acct where id = 1"), std::string("100"));
  CHECK_EQ(db.One("select bal from acct where id = 1", other.get()), std::string("70"));
  CHECK_CODE(db.Run("update acct set bal = bal - 1 where id = 1"), state::kSerialization);
  db.Run("rollback");
  db.Run("begin");
  db.Run("update acct set bal = bal + 5 where id = 2");
  CHECK_EQ(db.One("select bal from acct where id = 2", other.get()), std::string("100"));
  db.Run("commit");
  CHECK_EQ(db.One("select bal from acct where id = 2", other.get()), std::string("105"));
}

TEST(errors_use_postgres_codes) {
  Sql db;
  CHECK_CODE(db.Run("selec 1"), state::kSyntax);
  CHECK_CODE(db.Run("select * from missing"), state::kNoTable);
  db.Run("create table e (id int primary key)");
  CHECK_CODE(db.Run("select nope from e"), state::kNoColumn);
  CHECK_CODE(db.Run("select * from e, e"), state::kNotSupported);
  CHECK_CODE(db.Run("select $1"), state::kNoParam);
  CHECK_CODE(db.Run("insert into e values (1, 2)"), state::kSyntax);
  CHECK_CODE(db.Run("create table d (a int default 1)"), state::kNotSupported);
  CHECK_CODE(db.Run("show nonsense"), "42704");
}

TEST(tables_without_a_primary_key_get_row_ids) {
  Sql db;
  db.Run("create table h (tid int, delta int, mtime timestamp)");
  for (int i = 0; i < 5; i++)
    db.Run("insert into h values (1, " + std::to_string(i) + ", current_timestamp)");
  CHECK_EQ(db.One("select count(*), sum(delta) from h"), std::string("5|10"));
  CHECK_EQ(db.One("select count(*) from h where mtime <= now()"), std::string("5"));
  CHECK_EQ(db.Rows("explain select * from h where tid = 1")[0], std::string("Seq Scan on h"));
}

TEST(select_without_a_table) {
  Sql db;
  CHECK_EQ(db.One("select 1"), std::string("1"));
  CHECK(db.Rows("select 1 where false").empty());
  CHECK_EQ(db.One("select version() like 'PostgreSQL%'"), std::string("t"));
  CHECK_EQ(db.Run("select 1 as a, 2").columns[1].name, std::string("?column?"));
}
