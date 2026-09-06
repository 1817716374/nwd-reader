#include "internal.hpp"
#include "sqlite/sqlite3.h"
#include <climits>
namespace nwd::detail {
namespace {
using Connection = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
Statement query(sqlite3 *db, const std::string &sql) {
  sqlite3_stmt *p = nullptr;
  int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &p, nullptr);
  Statement result(p, sqlite3_finalize);
  require(rc == SQLITE_OK,
          std::string("database query: ") + sqlite3_errmsg(db));
  return result;
}
bool next(sqlite3_stmt *p) {
  int rc = sqlite3_step(p);
  require(rc == SQLITE_ROW || rc == SQLITE_DONE,
          std::string("database read: ") +
              sqlite3_errmsg(sqlite3_db_handle(p)));
  return rc == SQLITE_ROW;
}
std::string text(sqlite3_stmt *s, int i) {
  auto p = sqlite3_column_text(s, i);
  return p ? std::string(reinterpret_cast<const char *>(p),
                         sqlite3_column_bytes(s, i))
           : std::string{};
}
std::string identifier(const std::string &name) {
  require(name.find('\0') == name.npos, "database identifier contains NUL");
  std::string s = "\"";
  for (auto c : name) {
    s += c;
    if (c == '"')
      s += c;
  }
  return s + '"';
}
uint32_t be32(std::span<const uint8_t> b, size_t i) {
  return uint32_t(b[i]) << 24 | uint32_t(b[i + 1]) << 16 |
         uint32_t(b[i + 2]) << 8 | b[i + 3];
}
} // namespace
void read_file_database(FileDatabase &out, Cursor &r, bool wrapped,
                        const Options &o) {
  uint64_t size = r.data.size() - r.pos;
  if (wrapped) {
    out.prefix = r.string();
    out.suffix = r.string();
    size = r.u64();
  }
  auto bytes = r.raw(size);
  require(size >= 100 && size <= o.max_decoded_chunk && size <= INT64_MAX &&
              !std::memcmp(bytes.data(), "SQLite format 3\0", 16),
          "database header/extent");
  out.page_size = uint32_t(bytes[16]) << 8 | bytes[17];
  if (out.page_size == 1)
    out.page_size = 65536;
  require(out.page_size >= 512 && out.page_size <= 65536 &&
              !(out.page_size & (out.page_size - 1)),
          "database page size");
  out.page_count = be32(bytes, 28);
  out.user_version = be32(bytes, 60);
  require(uint64_t(out.page_count) * out.page_size == size,
          "database page count/extent");
  sqlite3 *raw = nullptr;
  auto rc = sqlite3_open_v2(":memory:", &raw,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                SQLITE_OPEN_NOMUTEX,
                            nullptr);
  Connection db(raw, sqlite3_close);
  require(rc == SQLITE_OK, "database memory connection");
  sqlite3_db_config(raw, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr);
  sqlite3_db_config(raw, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
  sqlite3_limit(
      raw, SQLITE_LIMIT_LENGTH,
      static_cast<int>(std::min<uint64_t>(o.max_decoded_chunk, INT_MAX)));
  require(sqlite3_deserialize(raw, "main",
                              const_cast<unsigned char *>(bytes.data()), size,
                              size, SQLITE_DESERIALIZE_READONLY) == SQLITE_OK,
          "database deserialize");
  uint64_t operations =
      std::min<uint64_t>(o.max_objects, (UINT64_MAX - 10000) / 100) * 100 +
      10000;
  sqlite3_progress_handler(
      raw, 1000,
      [](void *p) {
        auto &n = *static_cast<uint64_t *>(p);
        if (n < 1000)
          return 1;
        n -= 1000;
        return 0;
      },
      &operations);
  uint64_t cells = 0, payload = 0;
  auto charge = [&](uint64_t n = 0) {
    require(cells < o.max_objects && n <= o.max_decoded_chunk - payload,
            "database output resource limit");
    ++cells;
    payload += n;
  };
  auto schema = query(raw, "SELECT type,name,tbl_name,rootpage,sql FROM "
                           "sqlite_schema ORDER BY rowid");
  while (next(schema.get())) {
    auto &s = out.schema.emplace_back();
    s.type = text(schema.get(), 0);
    s.name = text(schema.get(), 1);
    s.table = text(schema.get(), 2);
    s.root_page = sqlite3_column_int64(schema.get(), 3);
    if (sqlite3_column_type(schema.get(), 4) != SQLITE_NULL)
      s.sql = text(schema.get(), 4);
    charge(s.type.size() + s.name.size() + s.table.size() +
           (s.sql ? s.sql->size() : 0));
  }
  for (const auto &s : out.schema) {
    if (s.type != "table")
      continue;
    if (!s.root_page)
      throw UnsupportedLayout("database virtual table " + s.name);
    auto &table = out.tables.emplace_back();
    table.name = s.name;
    auto rows = query(raw, "SELECT * FROM " + identifier(s.name));
    int n = sqlite3_column_count(rows.get());
    for (int i = 0; i < n; ++i) {
      table.columns.emplace_back(sqlite3_column_name(rows.get(), i));
      charge(table.columns.back().size());
    }
    while (next(rows.get())) {
      auto &row = table.rows.emplace_back();
      for (int i = 0; i < n; ++i) {
        auto &cell = row.emplace_back();
        auto type = sqlite3_column_type(rows.get(), i);
        switch (type) {
        case SQLITE_NULL:
          charge();
          break;
        case SQLITE_INTEGER:
          charge();
          cell = sqlite3_column_int64(rows.get(), i);
          break;
        case SQLITE_FLOAT:
          charge();
          cell = sqlite3_column_double(rows.get(), i);
          break;
        case SQLITE_TEXT:
          charge(sqlite3_column_bytes(rows.get(), i));
          cell = text(rows.get(), i);
          break;
        case SQLITE_BLOB: {
          auto length = sqlite3_column_bytes(rows.get(), i);
          charge(length);
          auto p =
              static_cast<const uint8_t *>(sqlite3_column_blob(rows.get(), i));
          auto &v = cell.emplace<std::vector<uint8_t>>();
          if (length)
            v.assign(p, p + length);
          break;
        }
        default:
          throw Error("database cell type");
        }
      }
    }
    auto keys =
        query(raw, "PRAGMA foreign_key_list(" + identifier(s.name) + ")");
    while (next(keys.get())) {
      auto &k = table.foreign_keys.emplace_back();
      k.id = sqlite3_column_int64(keys.get(), 0);
      k.sequence = sqlite3_column_int64(keys.get(), 1);
      k.table = text(keys.get(), 2);
      k.from = text(keys.get(), 3);
      if (sqlite3_column_type(keys.get(), 4) != SQLITE_NULL)
        k.to = text(keys.get(), 4);
      k.on_update = text(keys.get(), 5);
      k.on_delete = text(keys.get(), 6);
      k.match = text(keys.get(), 7);
      charge(k.table.size() + k.from.size() + (k.to ? k.to->size() : 0) +
             k.on_update.size() + k.on_delete.size() + k.match.size());
    }
  }
}
} // namespace nwd::detail
