// Extra tests for simple_orm: the edge cases listed in the README and a few more.
// Run it in an empty directory: it creates and deletes json files there.
//
// Two table types with the same name() share one file but not their in-memory state;
// that is used to simulate "a fresh process reads what another one wrote".

#include "simple_orm.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono;

static int failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " << #cond << '\n'; \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

#define CHECK_THROWS(expr)                                                            \
    do {                                                                              \
        bool thrown_ = false;                                                         \
        try {                                                                         \
            (void)(expr);                                                             \
        } catch (const std::invalid_argument&) {                                      \
            thrown_ = true;                                                           \
        }                                                                             \
        if (!thrown_) {                                                               \
            std::cerr << __FILE__ << ':' << __LINE__ << ": expected std::invalid_argument: " \
                      << #expr << '\n';                                               \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

static std::string slurp(const std::string& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void spit(const std::string& file, const std::string& text) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
}

static std::vector<std::string> lines_of(const std::string& file) {
    std::ifstream in(file);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);)
        if (!line.empty()) lines.push_back(line);
    return lines;
}

static bool have_jq() { return std::system("command -v jq > /dev/null 2>&1") == 0; }

static bool jq_valid(const std::string& file) {
    if (!have_jq()) return true;
    return std::system(("jq . " + file + " > /dev/null 2>&1").c_str()) == 0;
}

static std::string dbl(double v) {
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, r.ptr);
}

template <class R>
static std::vector<std::int64_t> ids_of(const R& result) {
    std::vector<std::int64_t> ids;
    for (auto* item : result) ids.push_back(item->id_.get());
    return ids;
}
using Ids = std::vector<std::int64_t>;

static Ids sorted(Ids v) {
    std::sort(v.begin(), v.end());
    return v;
}

// An integer-keyed table with one field of every scalar type.
#define DEFINE_ITEM(NAME, FILE)                                                     \
    struct NAME : public DBTableType {                                              \
        IntType id_{"id"};                                                          \
        StringType text_{"text"};                                                   \
        FloatType f_{"f"};                                                          \
        DateType d_{"d"};                                                           \
        NAME() : DBTableType(PrimaryKey(&id_), &text_, &f_, &d_) {}                 \
        constexpr static std::string_view name() { return FILE; }                   \
    };

// A string-keyed table.
#define DEFINE_NAMED(NAME, FILE)                                                    \
    struct NAME : public DBTableType {                                              \
        StringType key_{"key"};                                                     \
        IntType n_{"n"};                                                            \
        NAME() : DBTableType(PrimaryKey(&key_), &n_) {}                             \
        constexpr static std::string_view name() { return FILE; }                   \
    };

template <class I>
static I* make_item(std::int64_t id, const std::string& text, double f, year_month_day d) {
    I* item = DBManager<I>::create();
    item->id_ = id;
    item->text_ = text;
    item->f_ = f;
    item->d_ = d;
    return item;
}

static void clean(std::initializer_list<const char*> files) {
    for (const char* f : files) fs::remove(f);
}

// ---------------------------------------------------------------------------
DEFINE_ITEM(MissingItem, "MissingItem")
DEFINE_ITEM(EmptyItem, "EmptyItem")

static void test_missing_and_empty_files() {
    clean({"MissingItem.json", "EmptyItem.json"});
    CHECK(DBManager<MissingItem>::search(1) == nullptr);
    CHECK(DBManager<MissingItem>::search("text", EQUAL_TO("a")).size() == 0);
    DBManager<MissingItem>::load();
    DBManager<MissingItem>::save();
    CHECK(!fs::exists("MissingItem.json"));

    spit("EmptyItem.json", "");
    CHECK(DBManager<EmptyItem>::search(1) == nullptr);
    CHECK(DBManager<EmptyItem>::search("id", GREATER_THAN(0)).size() == 0);
    EmptyItem* e = make_item<EmptyItem>(1, "x", 2.5, 2020y / March / 4);
    e->save();
    CHECK((lines_of("EmptyItem.json") ==
           std::vector<std::string>{R"({"id": 1, "text": "x", "f": 2.5, "d": "2020-03-04"})"}));
    CHECK(jq_valid("EmptyItem.json"));
}

// ---------------------------------------------------------------------------
DEFINE_ITEM(WItem, "RoundTrip")
DEFINE_ITEM(RItem, "RoundTrip")

static void test_value_formats_and_round_trip() {
    clean({"RoundTrip.json"});
    const std::vector<std::string> strings = {
        "",
        "plain",
        "quote \" and backslash \\ and slash /",
        "newline\n tab\t cr\r bs\b ff\f",
        std::string("ctl\x01\x1f end"),
        std::string("nul\0inside", 10),
        "한글 홍길동 😀 é",
        std::string(20000, 'x'),
        "\\n is not a newline",
        "\"\"\"",
    };
    const std::vector<double> doubles = {0.1,  1.0,  -0.0, 1e300,  5e-324,
                                         123456789.125, 100000.0, 1e21, 1e-7, 3.141592653589793,
                                         -2.5, 0.0,  1.0 / 3.0,
                                         std::numeric_limits<double>::max(),
                                         std::numeric_limits<double>::min()};
    const std::vector<std::int64_t> big_ids = {-1, 9223372036854775807LL,
                                               std::numeric_limits<std::int64_t>::min(), -1000000, 0};

    std::int64_t id = 100;
    for (const std::string& s : strings) make_item<WItem>(++id, s, 0.5, 1999y / December / 31);
    for (double v : doubles) make_item<WItem>(++id, "d", v, 2000y / January / 1);
    for (std::int64_t v : big_ids) make_item<WItem>(v, "big", 1, 1970y / January / 1);
    make_item<WItem>(7, "dates", 0, 99y / January / 5);  // year 0099
    DBManager<WItem>::save();

    CHECK(jq_valid("RoundTrip.json"));
    const auto lines = lines_of("RoundTrip.json");
    CHECK(lines.size() == strings.size() + doubles.size() + big_ids.size() + 1);

    // exact text of a few lines
    CHECK(lines[0] == R"({"id": 101, "text": "", "f": 0.5, "d": "1999-12-31"})");
    CHECK(lines[2] == R"({"id": 103, "text": "quote \" and backslash \\ and slash /", "f": 0.5, "d": "1999-12-31"})");
    CHECK(lines[3] == R"({"id": 104, "text": "newline\n tab\t cr\r bs\b ff\f", "f": 0.5, "d": "1999-12-31"})");
    CHECK(lines[4] == R"({"id": 105, "text": "ctl\u0001\u001f end", "f": 0.5, "d": "1999-12-31"})");
    CHECK(lines[5] == R"({"id": 106, "text": "nul\u0000inside", "f": 0.5, "d": "1999-12-31"})");
    CHECK(lines[6] == R"({"id": 107, "text": "한글 홍길동 😀 é", "f": 0.5, "d": "1999-12-31"})");
    CHECK(lines[10 + 0].find(R"("f": 0.1,)") != std::string::npos);
    CHECK(lines[10 + 1].find(R"("f": 1,)") != std::string::npos);
    CHECK(lines[10 + 2].find(R"("f": -0,)") != std::string::npos);
    CHECK(lines[10 + 3].find("\"f\": " + dbl(1e300) + ",") != std::string::npos);
    CHECK(lines.back() == R"({"id": 7, "text": "dates", "f": 0, "d": "0099-01-05"})");
    CHECK(lines[strings.size() + doubles.size() + 1] ==
          R"({"id": 9223372036854775807, "text": "big", "f": 1, "d": "1970-01-01"})");
    CHECK(lines[strings.size() + doubles.size() + 2] ==
          R"({"id": -9223372036854775808, "text": "big", "f": 1, "d": "1970-01-01"})");

    // read everything back through a second table that has no objects in memory
    id = 100;
    for (const std::string& s : strings) {
        RItem* r = DBManager<RItem>::search(++id);
        CHECK(r != nullptr && r->text_.get() == s);
        CHECK(r != nullptr && r->f_.get() == 0.5);
        CHECK(r != nullptr && r->d_.get() == 1999y / December / 31);
    }
    for (double v : doubles) {
        RItem* r = DBManager<RItem>::search(++id);
        CHECK(r != nullptr);
        if (r != nullptr) {
            CHECK(r->f_.get() == v);
            CHECK(std::signbit(r->f_.get()) == std::signbit(v));
        }
    }
    for (std::int64_t v : big_ids) {
        RItem* r = DBManager<RItem>::search(v);
        CHECK(r != nullptr && r->id_.get() == v);
    }
    RItem* dated = DBManager<RItem>::search(7);
    CHECK(dated != nullptr && dated->d_.get() == 99y / January / 5);
    CHECK(DBManager<RItem>::search(8) == nullptr);
}

// ---------------------------------------------------------------------------
DEFINE_ITEM(ParseItem, "ParseItem")

static void test_parsing_files_written_by_others() {
    // fields in another order, an unknown field, extra spaces, \u escapes, a surrogate pair,
    // a CRLF line, an empty line, no newline at the end
    spit("ParseItem.json",
         "{\"text\": \"caf\\u00e9 \\ud83d\\ude00\", \"id\": 1, \"f\": 1e2, \"d\": \"2024-02-29\", \"extra\": [1, {\"a\": \"}\"}]}\n"
         "\n"
         "{  \"id\" : 2 ,\"text\":\"b\",\"f\":-0.5,\"d\":\"2000-01-01\"  }\r\n"
         "{\"id\": 3, \"text\": \"last line has no newline\"}");
    ParseItem* a = DBManager<ParseItem>::search(1);
    CHECK(a != nullptr);
    if (a != nullptr) {
        CHECK(a->text_.get() == "caf\xc3\xa9 \xf0\x9f\x98\x80");
        CHECK(a->f_.get() == 100.0);
        CHECK(a->d_.get() == 2024y / February / 29);
    }
    ParseItem* b = DBManager<ParseItem>::search(2);
    CHECK(b != nullptr && b->text_.get() == "b" && b->f_.get() == -0.5);
    ParseItem* c = DBManager<ParseItem>::search(3);
    CHECK(c != nullptr && c->text_.get() == "last line has no newline");
    CHECK(c != nullptr && c->f_.get() == 0.0 && c->d_.get() == 1970y / January / 1);

    // saving into such a file normalises it, and never glues two lines together
    ParseItem* d = make_item<ParseItem>(4, "new", 1, 2001y / January / 1);
    d->save();
    const auto lines = lines_of("ParseItem.json");
    CHECK(lines.size() == 4);
    CHECK(!lines.empty() && lines.back() == R"({"id": 4, "text": "new", "f": 1, "d": "2001-01-01"})");
    CHECK(jq_valid("ParseItem.json"));
}

// ---------------------------------------------------------------------------
DEFINE_ITEM(SaveItem, "SaveItem")
DEFINE_ITEM(SaveItemReader, "SaveItem")

static void test_saving_semantics() {
    clean({"SaveItem.json"});
    SaveItem* a = make_item<SaveItem>(1, "aaa", 1, 2020y / January / 1);
    SaveItem* b = make_item<SaveItem>(2, "bbb", 2, 2020y / January / 2);
    SaveItem* c = make_item<SaveItem>(3, "ccc", 3, 2020y / January / 3);
    a->save();
    b->save();
    c->save();
    const std::string before = slurp("SaveItem.json");
    CHECK(lines_of("SaveItem.json").size() == 3);

    // saving unchanged objects (single and bulk) leaves the file untouched
    const auto mtime = fs::last_write_time("SaveItem.json");
    a->save();
    a->save();
    DBManager<SaveItem>::save();
    CHECK(slurp("SaveItem.json") == before);
    CHECK(fs::last_write_time("SaveItem.json") == mtime);

    // same-length change: patched in place
    b->text_ = "xyz";
    b->save();
    auto lines = lines_of("SaveItem.json");
    CHECK(lines.size() == 3 && lines[1] == R"({"id": 2, "text": "xyz", "f": 2, "d": "2020-01-02"})");
    CHECK(lines[0] == R"({"id": 1, "text": "aaa", "f": 1, "d": "2020-01-01"})");

    // different-length change in the middle: the line keeps its position
    b->text_ = "a much longer text than before";
    b->save();
    lines = lines_of("SaveItem.json");
    CHECK(lines.size() == 3);
    CHECK(lines.size() == 3 && lines[1].find("a much longer text") != std::string::npos);
    CHECK(lines.size() == 3 && lines[2] == R"({"id": 3, "text": "ccc", "f": 3, "d": "2020-01-03"})");

    // shrinking again, then several changes at once through the bulk save
    b->text_ = "s";
    a->f_ = 10;      // same length "1" -> "10" is longer, so this rewrites
    c->text_ = "CCC";  // same length
    DBManager<SaveItem>::save();
    lines = lines_of("SaveItem.json");
    CHECK((lines == std::vector<std::string>{R"({"id": 1, "text": "aaa", "f": 10, "d": "2020-01-01"})",
                                              R"({"id": 2, "text": "s", "f": 2, "d": "2020-01-02"})",
                                              R"({"id": 3, "text": "CCC", "f": 3, "d": "2020-01-03"})"}));

    // appends and same-length patches in one bulk save
    SaveItem* d = make_item<SaveItem>(4, "ddd", 4, 2020y / January / 4);
    (void)d;
    c->text_ = "ccc";
    DBManager<SaveItem>::save();
    lines = lines_of("SaveItem.json");
    CHECK(lines.size() == 4);
    CHECK(lines.size() == 4 && lines[2] == R"({"id": 3, "text": "ccc", "f": 3, "d": "2020-01-03"})");
    CHECK(lines.size() == 4 && lines[3] == R"({"id": 4, "text": "ddd", "f": 4, "d": "2020-01-04"})");
    CHECK(jq_valid("SaveItem.json"));

    // a second table sees exactly what was written
    SaveItemReader* r = DBManager<SaveItemReader>::search(3);
    CHECK(r != nullptr && r->text_.get() == "ccc");
    CHECK(DBManager<SaveItemReader>::search("id", GREATER_THAN(0)).size() == 4);

    // a line appended by somebody else is noticed
    {
        std::ofstream out("SaveItem.json", std::ios::app);
        out << R"({"id": 50, "text": "external", "f": 0, "d": "2020-01-01"})" << '\n';
    }
    SaveItem* ext = DBManager<SaveItem>::search(50);
    CHECK(ext != nullptr && ext->text_.get() == "external");
    // ... and saving afterwards keeps it
    a->text_ = "AAA";
    a->save();
    CHECK(lines_of("SaveItem.json").size() == 5);

    // objects are kept in memory when the file is deleted; new saves recreate it
    const std::size_t n = lines_of("SaveItem.json").size();
    fs::remove("SaveItem.json");
    CHECK(DBManager<SaveItem>::search(1) == a);
    a->text_ = "again";
    a->save();
    CHECK(lines_of("SaveItem.json").size() == 1);
    (void)n;
}

// ---------------------------------------------------------------------------
DEFINE_NAMED(DupTable, "DupTable")
DEFINE_NAMED(DupTableOther, "DupTable")
DEFINE_NAMED(DupBulk, "DupBulk")
DEFINE_NAMED(DupUnsaved, "DupUnsaved")

static DupTable* named(const std::string& key, std::int64_t n) {
    DupTable* t = DBManager<DupTable>::create();
    t->key_ = key;
    t->n_ = n;
    return t;
}

static void test_duplicate_keys() {
    clean({"DupTable.json", "DupBulk.json", "DupUnsaved.json"});
    DupTable* a = named("k1", 1);
    a->save();
    DupTable* b = named("k1", 2);
    bool thrown = false;
    try {
        b->save();
    } catch (const std::invalid_argument&) {
        thrown = true;
    }
    CHECK(thrown);
    CHECK(lines_of("DupTable.json").size() == 1);
    CHECK(slurp("DupTable.json") == "{\"key\": \"k1\", \"n\": 1}\n");
    CHECK(DBManager<DupTable>::search("k1") == a);  // the saved one wins
    b->key_ = "k2";
    b->save();
    CHECK(lines_of("DupTable.json").size() == 2);
    CHECK(DBManager<DupTable>::search("k2") == b);

    // conflict with a line in the file that is not loaded in memory
    DupTableOther* o = DBManager<DupTableOther>::create();
    o->key_ = "k1";
    thrown = false;
    try {
        o->save();
    } catch (const std::invalid_argument&) {
        thrown = true;
    }
    CHECK(thrown);
    CHECK(lines_of("DupTable.json").size() == 2);
    // an object in memory is found by search even though it is not saved
    CHECK(DBManager<DupTableOther>::search("k1") == o);

    // bulk save: one conflict means nothing is written
    DupBulk* x = DBManager<DupBulk>::create();
    x->key_ = "x";
    x->save();
    const std::string before = slurp("DupBulk.json");
    DupBulk* y1 = DBManager<DupBulk>::create();
    y1->key_ = "y";
    DupBulk* y2 = DBManager<DupBulk>::create();
    y2->key_ = "y";
    DupBulk* z = DBManager<DupBulk>::create();
    z->key_ = "z";
    thrown = false;
    try {
        DBManager<DupBulk>::save();
    } catch (const std::invalid_argument&) {
        thrown = true;
    }
    CHECK(thrown);
    CHECK(slurp("DupBulk.json") == before);
    y2->key_ = "y2";
    DBManager<DupBulk>::save();
    CHECK(lines_of("DupBulk.json").size() == 4);

    // two unsaved objects with the same key: the first save wins
    DupUnsaved* u1 = DBManager<DupUnsaved>::create();
    DupUnsaved* u2 = DBManager<DupUnsaved>::create();
    u1->key_ = "same";
    u2->key_ = "same";
    u1->save();
    thrown = false;
    try {
        u2->save();
    } catch (const std::invalid_argument&) {
        thrown = true;
    }
    CHECK(thrown);
    CHECK(lines_of("DupUnsaved.json").size() == 1);
}

// ---------------------------------------------------------------------------
struct CntA : public DBTableType {
    IntType id_ = DBManager<CntA>::get_next_key("id");
    StringType note_{"note"};
    CntA() : DBTableType(PrimaryKey(&id_), &note_) {}
    constexpr static std::string_view name() { return "CntA"; }
};
struct CntB : public DBTableType {
    IntType id_ = DBManager<CntB>::get_next_key("id");
    CntB() : DBTableType(PrimaryKey(&id_)) {}
    constexpr static std::string_view name() { return "CntB"; }
};
struct CntC : public DBTableType {
    IntType id_ = DBManager<CntC>::get_next_key("id");
    CntC() : DBTableType(PrimaryKey(&id_)) {}
    constexpr static std::string_view name() { return "CntC"; }
};

static void test_get_next_key() {
    clean({"CntA.json", "CntB.json", "CntC.json"});
    spit("CntA.json", "{\"id\": 5, \"note\": \"five\"}\n{\"id\": 7, \"note\": \"seven\"}\n");
    CHECK(DBManager<CntA>::search(6) == nullptr);       // a failed search uses no key
    CntA* a1 = DBManager<CntA>::create();
    CHECK(a1->id_.get() == 8);
    CntA* a2 = DBManager<CntA>::create();               // new value even though a1 is unsaved
    CHECK(a2->id_.get() == 9);
    CntA* five = DBManager<CntA>::search(5);            // loading uses no key
    CHECK(five != nullptr && five->note_.get() == "five");
    CHECK(DBManager<CntA>::search("note", EQUAL_TO("seven")).size() == 1);  // loads everything
    CntA* a3 = DBManager<CntA>::create();
    CHECK(a3->id_.get() == 10);
    a3->save();
    CntA* a4 = DBManager<CntA>::create();
    CHECK(a4->id_.get() == 11);
    CHECK(lines_of("CntA.json").size() == 3);

    CntB* b1 = DBManager<CntB>::create();
    CntB* b2 = DBManager<CntB>::create();
    CHECK(b1->id_.get() == 1 && b2->id_.get() == 2);
    b2->save();
    CHECK(slurp("CntB.json") == "{\"id\": 2}\n");
    CHECK(DBManager<CntB>::create()->id_.get() == 3);

    spit("CntC.json", "{\"id\": -5}\n");
    CHECK(DBManager<CntC>::create()->id_.get() == -4);
    CHECK(DBManager<CntC>::create()->id_.get() == -3);

    CHECK(DBManager<CntA>::get_next_key("id").get() > 11);
}

// ---------------------------------------------------------------------------
DEFINE_NAMED(FkTarget, "FkTarget")
DEFINE_ITEM(FkNum, "FkNum")

struct FkOwner : public DBTableType {
    IntType id_{"id"};
    StringType label_{"label"};
    ForeignKey<FkTarget> one_{"one"};
    ForeignKeyVector<FkTarget> many_{"many"};
    ForeignKey<FkNum> num_{"num"};
    ForeignKeyVector<FkNum> nums_{"nums"};
    FkOwner() : DBTableType(PrimaryKey(&id_), &label_, &one_, &many_, &num_, &nums_) {}
    constexpr static std::string_view name() { return "FkOwner"; }
};
struct FkOwnerReader : public DBTableType {
    IntType id_{"id"};
    StringType label_{"label"};
    ForeignKey<FkTarget> one_{"one"};
    ForeignKeyVector<FkTarget> many_{"many"};
    ForeignKey<FkNum> num_{"num"};
    ForeignKeyVector<FkNum> nums_{"nums"};
    FkOwnerReader() : DBTableType(PrimaryKey(&id_), &label_, &one_, &many_, &num_, &nums_) {}
    constexpr static std::string_view name() { return "FkOwner"; }
};

static void test_foreign_keys() {
    clean({"FkTarget.json", "FkNum.json", "FkOwner.json"});
    FkTarget* t1 = DBManager<FkTarget>::create();
    t1->key_ = "t1";
    t1->save();
    FkTarget* t2 = DBManager<FkTarget>::create();
    t2->key_ = "t \"2\"";
    t2->save();
    FkNum* n1 = make_item<FkNum>(11, "n", 0, 2020y / January / 1);
    n1->save();

    FkOwner* o = DBManager<FkOwner>::create();
    o->id_ = 1;
    o->label_ = "owner";
    CHECK(o->one_.get() == nullptr);
    CHECK(o->many_.get().empty());
    o->save();
    CHECK(lines_of("FkOwner.json")[0] ==
          R"({"id": 1, "label": "owner", "one": null, "many": [], "num": null, "nums": []})");

    o->one_ = t2;
    o->many_.push_back(t1);
    o->many_.push_back(t2);
    o->many_.push_back(t1);  // the same object twice
    o->many_.push_back(nullptr);  // ignored
    o->num_ = n1;
    o->nums_.push_back(n1);
    o->save();
    CHECK(lines_of("FkOwner.json")[0] ==
          R"({"id": 1, "label": "owner", "one": "t \"2\"", "many": ["t1", "t \"2\"", "t1"], "num": 11, "nums": [11]})");
    CHECK(o->one_.get() == t2);
    CHECK((o->many_.get() == std::vector<FkTarget*>{t1, t2, t1}));
    CHECK(o->num_.get() == n1);

    o->one_ = nullptr;
    o->save();
    CHECK(lines_of("FkOwner.json")[0].find(R"("one": null,)") != std::string::npos);

    // foreign key fields cannot be searched or ordered by
    CHECK_THROWS(DBManager<FkOwner>::search("one", EQUAL_TO("t1")));
    CHECK_THROWS(DBManager<FkOwner>::search("many", EQUAL_TO(1)));
    CHECK_THROWS(DBManager<FkOwner>::search("id", GREATER_THAN(0)).order_by("one"));

    // a second table reads the owner from the file and resolves the references
    FkOwnerReader* r = DBManager<FkOwnerReader>::search(1);
    CHECK(r != nullptr);
    if (r != nullptr) {
        CHECK(r->one_.get() == nullptr);
        auto many = r->many_.get();
        CHECK(many.size() == 3 && many[0] == many[2] && many[0] != many[1]);
        CHECK(many.size() == 3 && many[0]->key_.get() == "t1" && many[1]->key_.get() == "t \"2\"");
        CHECK(r->num_.get() != nullptr && r->num_.get()->id_.get() == 11);
        CHECK(r->nums_.get().size() == 1);
        CHECK(DBManager<FkTarget>::search("t1") == many[0]);  // same object as search(key)
    }

    // dangling references: the key's object does not exist (any more)
    spit("FkOwner.json",
         "{\"id\": 2, \"label\": \"dangling\", \"one\": \"missing\", \"many\": [\"t1\", \"missing\", \"t1\"], "
         "\"num\": 999, \"nums\": [999, 11]}\n");
    FkOwner* d = DBManager<FkOwner>::search(2);
    CHECK(d != nullptr);
    if (d != nullptr) {
        CHECK(d->one_.get() == nullptr);
        CHECK((d->many_.get() == std::vector<FkTarget*>{t1, t1}));
        CHECK(d->num_.get() == nullptr);
        CHECK(d->nums_.get().size() == 1 && d->nums_.get()[0] == n1);
        // saving an unrelated change keeps the dangling keys as they were
        d->label_ = "dangling, edited";
        d->save();
        const auto lines = lines_of("FkOwner.json");
        CHECK(lines.size() >= 1);
        CHECK(lines.back() ==
              R"({"id": 2, "label": "dangling, edited", "one": "missing", "many": ["t1", "missing", "t1"], "num": 999, "nums": [999, 11]})");
        CHECK(jq_valid("FkOwner.json"));
    }
}

// ---------------------------------------------------------------------------
struct MClass;
struct MStudent : public DBTableType {
    StringType sid_{"sid"};
    ForeignKeyVector<MClass> classes_{"classes"};
    ForeignKey<MClass> favourite_{"favourite"};
    MStudent() : DBTableType(PrimaryKey(&sid_), &classes_, &favourite_) {}
    constexpr static std::string_view name() { return "MStudent"; }
};
struct MClass : public DBTableType {
    IntType id_{"id"};
    ForeignKeyVector<MStudent> students_{"students"};
    MClass() : DBTableType(PrimaryKey(&id_), &students_) {}
    constexpr static std::string_view name() { return "MClass"; }
};

static void test_mutual_references() {
    clean({"MStudent.json", "MClass.json"});
    MStudent* s = DBManager<MStudent>::create();
    s->sid_ = "s1";
    MClass* c = DBManager<MClass>::create();
    c->id_ = 5;
    c->students_.push_back(s);
    s->classes_.push_back(c);
    s->favourite_ = c;
    s->save();
    c->save();
    CHECK(slurp("MStudent.json") == "{\"sid\": \"s1\", \"classes\": [5], \"favourite\": 5}\n");
    CHECK(slurp("MClass.json") == "{\"id\": 5, \"students\": [\"s1\"]}\n");
    CHECK(s->classes_.get()[0]->students_.get()[0] == s);
    CHECK(s->favourite_.get() == c);
}

// ---------------------------------------------------------------------------
DEFINE_ITEM(SearchItem, "SearchItem")
DEFINE_ITEM(SearchItemReader, "SearchItem")

static void test_advanced_search() {
    clean({"SearchItem.json"});
    const std::vector<std::string> text = {"b", "a", "c", "a", "b", "d"};
    const std::vector<double> f = {1.5, -2, 3, 1.5, 0, 10};
    const std::vector<year_month_day> d = {2020y / January / 1, 2021y / May / 5,  2019y / December / 31,
                                           2021y / May / 5,     2022y / February / 2, 2020y / January / 1};
    for (int i = 0; i < 6; ++i) make_item<SearchItem>(i + 1, text[i], f[i], d[i]);
    DBManager<SearchItem>::save();
    using S = DBManager<SearchItem>;
    using SR = DBManager<SearchItemReader>;

    // the same answers whether the objects come from memory or from the file
    auto run = [&]<class M>() {
        CHECK((sorted(ids_of(M::search("text", EQUAL_TO("a")))) == Ids{2, 4}));
        CHECK((sorted(ids_of(M::search("text", GREATER_THAN("b")))) == Ids{3, 6}));
        CHECK((sorted(ids_of(M::search("text", LESS_THAN("b")))) == Ids{2, 4}));
        CHECK((sorted(ids_of(M::search("text", GREATER_THAN_OR_EQUAL_TO("b")))) == Ids{1, 3, 5, 6}));
        CHECK((sorted(ids_of(M::search("text", LESS_THAN_OR_EQUAL_TO("b")))) == Ids{1, 2, 4, 5}));
        CHECK((sorted(ids_of(M::search("text", AND(GREATER_THAN_OR_EQUAL_TO("a"), LESS_THAN("c"))))) ==
               Ids{1, 2, 4, 5}));
        CHECK((sorted(ids_of(M::search("text", OR(EQUAL_TO("a"), EQUAL_TO("d"))))) == Ids{2, 4, 6}));
        CHECK((sorted(ids_of(M::search("text", NOT(EQUAL_TO("a"))))) == Ids{1, 3, 5, 6}));
        CHECK((sorted(ids_of(M::search("text", NOT(NOT(EQUAL_TO("a")))))) == Ids{2, 4}));
        CHECK((sorted(ids_of(M::search("f", GREATER_THAN(1)))) == Ids{1, 3, 4, 6}));
        CHECK((sorted(ids_of(M::search("f", EQUAL_TO(1.5)))) == Ids{1, 4}));
        CHECK((sorted(ids_of(M::search("f", LESS_THAN(0)))) == Ids{2}));
        CHECK((sorted(ids_of(M::search("f", EQUAL_TO(3)))) == Ids{3}));
        CHECK((sorted(ids_of(M::search("id", GREATER_THAN(4)))) == Ids{5, 6}));
        CHECK((sorted(ids_of(M::search("id", EQUAL_TO(3)))) == Ids{3}));
        CHECK((sorted(ids_of(M::search("d", EQUAL_TO(2021y / May / 5)))) == Ids{2, 4}));
        CHECK((sorted(ids_of(M::search("d", LESS_THAN(2020y / January / 2)))) == Ids{1, 3, 6}));
        CHECK((sorted(ids_of(M::search("d", GREATER_THAN_OR_EQUAL_TO(2021y / May / 5)))) == Ids{2, 4, 5}));
        // none / all
        CHECK(M::search("text", EQUAL_TO("nothing")).size() == 0);
        CHECK(M::search("text", GREATER_THAN("zzz")).empty());
        CHECK(M::search("id", GREATER_THAN(0)).size() == 6);
        CHECK(M::search("text", OR(EQUAL_TO("a"), NOT(EQUAL_TO("a")))).size() == 6);
        // order_by
        CHECK((ids_of(M::search("id", GREATER_THAN(0)).order_by("text", Ordering::Ascending)) ==
               Ids{2, 4, 1, 5, 3, 6}));
        CHECK((ids_of(M::search("id", GREATER_THAN(0)).order_by("text", Ordering::Descending)) ==
               Ids{6, 3, 1, 5, 2, 4}));
        CHECK((ids_of(M::search("id", GREATER_THAN(0)).order_by("f", Ordering::Ascending)) ==
               Ids{2, 5, 1, 4, 3, 6}));
        CHECK((ids_of(M::search("id", GREATER_THAN(0)).order_by("d", Ordering::Descending)) ==
               Ids{5, 2, 4, 1, 6, 3}));
        CHECK((ids_of(M::search("id", GREATER_THAN(0)).order_by("id", Ordering::Descending)) ==
               Ids{6, 5, 4, 3, 2, 1}));
        CHECK(M::search("text", EQUAL_TO("nothing")).order_by("id", Ordering::Descending).size() == 0);
        // same objects as search(key)
        for (auto* item : M::search("id", GREATER_THAN(0))) CHECK(M::search(item->id_.get()) == item);
        // type and name errors
        CHECK_THROWS(M::search("text", EQUAL_TO(5)));
        CHECK_THROWS(M::search("text", EQUAL_TO(2020y / January / 1)));
        CHECK_THROWS(M::search("f", EQUAL_TO("x")));
        CHECK_THROWS(M::search("id", LESS_THAN(2.5)));
        CHECK_THROWS(M::search("id", EQUAL_TO("1")));
        CHECK_THROWS(M::search("d", EQUAL_TO(5)));
        CHECK_THROWS(M::search("d", EQUAL_TO("2020-01-01")));
        CHECK_THROWS(M::search("nope", EQUAL_TO(1)));
        CHECK_THROWS(M::search("", EQUAL_TO(1)));
        CHECK_THROWS(M::search("text", AND(EQUAL_TO("a"), EQUAL_TO(1))));
        CHECK_THROWS(M::search("text", NOT(EQUAL_TO(1))));
        CHECK_THROWS(M::search("id", GREATER_THAN(0)).order_by("nope"));
    };
    run.operator()<S>();
    run.operator()<SR>();
}

// ---------------------------------------------------------------------------
static void test_memory_values_in_search() {
    // SearchItemReader has already loaded the file in test_advanced_search.
    using SR = DBManager<SearchItemReader>;
    SearchItemReader* two = SR::search(2);
    CHECK(two != nullptr && two->text_.get() == "a");
    two->text_ = "zzz";  // not saved
    CHECK((ids_of(SR::search("text", EQUAL_TO("a"))) == Ids{4}));
    CHECK((ids_of(SR::search("text", EQUAL_TO("zzz"))) == Ids{2}));
    SearchItemReader* fresh = make_item<SearchItemReader>(99, "a", 0, 2000y / January / 1);  // unsaved
    CHECK((sorted(ids_of(SR::search("text", EQUAL_TO("a")))) == Ids{4, 99}));
    CHECK(SR::search(99) == fresh);
    // load() keeps what is in memory (same pointers, in-memory values)
    SR::load();
    CHECK(SR::search(2) == two && two->text_.get() == "zzz");
    CHECK(SR::search(99) == fresh);
    // the file still has the old value
    CHECK(DBManager<SearchItem>::search(2)->text_.get() == "a");
}

// ---------------------------------------------------------------------------
DEFINE_ITEM(UnsavedItem, "UnsavedItem")
DEFINE_NAMED(StrTable, "StrTable")
DEFINE_ITEM(IntTable, "IntTable")

static void test_search_of_unsaved_objects() {
    clean({"UnsavedItem.json", "StrTable.json", "IntTable.json"});
    using U = DBManager<UnsavedItem>;
    UnsavedItem* a = U::create();
    CHECK(U::search(0) == a);  // default key
    a->id_ = 123;
    CHECK(U::search(123) == a);
    CHECK(U::search(0) == nullptr);
    a->id_ = 124;
    CHECK(U::search(123) == nullptr);
    CHECK(U::search(124) == a);
    a->save();
    CHECK(U::search(124) == a);
    CHECK(U::search(124) == U::search(124));
    CHECK(U::search(125) == nullptr);

    // many unsaved objects are searched quickly and correctly
    std::vector<UnsavedItem*> many;
    for (int i = 0; i < 2000; ++i) {
        UnsavedItem* m = U::create();
        m->id_ = 1000 + i;
        many.push_back(m);
    }
    bool all = true;
    for (int i = 0; i < 2000; ++i) all = all && U::search(1000 + i) == many[i];
    CHECK(all);

    // string keys
    StrTable* s = DBManager<StrTable>::create();
    s->key_ = "abc";
    CHECK(DBManager<StrTable>::search("abc") == s);
    CHECK(DBManager<StrTable>::search(std::string("abc")) == s);
    CHECK(DBManager<StrTable>::search(std::string_view("abc")) == s);
    CHECK(DBManager<StrTable>::search("ab") == nullptr);
    CHECK(DBManager<StrTable>::search(5) == nullptr);  // wrong key type
    IntTable* i = make_item<IntTable>(5, "x", 0, 2000y / January / 1);
    i->save();
    CHECK(DBManager<IntTable>::search(5) == i);
    CHECK(DBManager<IntTable>::search("5") == nullptr);  // wrong key type
}

// ---------------------------------------------------------------------------
static void test_output_operators() {
    using R = RItem;
    R* r = DBManager<R>::search(7);
    std::ostringstream os;
    if (r != nullptr) os << r->d_ << '|' << r->id_ << '|' << r->text_ << '|' << r->f_;
    CHECK(os.str() == "0099-01-05|7|dates|0");
    std::ostringstream os2;
    ParseItem* p = DBManager<ParseItem>::search(1);
    if (p != nullptr) os2 << p->d_;
    CHECK(os2.str() == "2024-02-29");

    // never-assigned fields
    DBManager<UnsavedItem>::create();
    UnsavedItem* u = DBManager<UnsavedItem>::create();
    CHECK(u->id_.get() == 0 && u->text_.get().empty() && u->f_.get() == 0.0);
    CHECK(u->d_.get() == 1970y / January / 1);
    std::ostringstream os3;
    os3 << u->d_ << ' ' << u->f_ << ' ' << u->id_ << '[' << u->text_ << ']';
    CHECK(os3.str() == "1970-01-01 0 0[]");
}

// ---------------------------------------------------------------------------
DEFINE_ITEM(JunkItem, "JunkItem")
DEFINE_ITEM(NoDirItem, "no_such_dir_for_simple_orm/NoDirItem")

static void test_junk_in_files_and_io_errors() {
    // blank lines, garbage, a line without the primary key and a repeated key: the first line
    // of a key wins; the junk is dropped the next time the file is written
    spit("JunkItem.json",
         "{\"id\": 1, \"text\": \"a\", \"f\": 0, \"d\": \"1970-01-01\"}\n"
         "not json at all\n"
         "{\"text\": \"no id\"}\n"
         "\n"
         "{\"id\": 1, \"text\": \"dup\", \"f\": 0, \"d\": \"1970-01-01\"}\n"
         "{\"id\": 2, \"text\": \"b\", \"f\": 0, \"d\": \"1970-01-01\"}\n");
    JunkItem* one = DBManager<JunkItem>::search(1);
    CHECK(one != nullptr && one->text_.get() == "a");
    CHECK(DBManager<JunkItem>::search(2) != nullptr);
    CHECK(DBManager<JunkItem>::search("id", GREATER_THAN(0)).size() == 2);
    make_item<JunkItem>(3, "c", 0, 1970y / January / 1)->save();
    CHECK(slurp("JunkItem.json") ==
          "{\"id\": 1, \"text\": \"a\", \"f\": 0, \"d\": \"1970-01-01\"}\n"
          "{\"id\": 2, \"text\": \"b\", \"f\": 0, \"d\": \"1970-01-01\"}\n"
          "{\"id\": 3, \"text\": \"c\", \"f\": 0, \"d\": \"1970-01-01\"}\n");
    CHECK(jq_valid("JunkItem.json"));

    // a file that cannot be written: std::runtime_error, nothing is lost afterwards
    fs::remove_all("no_such_dir_for_simple_orm");
    NoDirItem* item = make_item<NoDirItem>(1, "x", 0, 1970y / January / 1);
    bool thrown = false;
    try {
        item->save();
    } catch (const std::runtime_error&) {
        thrown = true;
    }
    CHECK(thrown);
    CHECK(DBManager<NoDirItem>::search(1) == item);
    fs::create_directory("no_such_dir_for_simple_orm");
    item->save();
    CHECK(lines_of("no_such_dir_for_simple_orm/NoDirItem.json").size() == 1);
    make_item<NoDirItem>(2, "y", 0, 1970y / January / 1);
    DBManager<NoDirItem>::save();
    CHECK(lines_of("no_such_dir_for_simple_orm/NoDirItem.json").size() == 2);
    fs::remove_all("no_such_dir_for_simple_orm");
}

// ---------------------------------------------------------------------------
// Randomised test of the file layout: creates, same-length and different-length changes,
// single and bulk saves.  After every step the file must equal the model's idea of it.
DEFINE_ITEM(FzItem, "Fuzz")

static void test_fuzz_file_layout() {
    clean({"Fuzz.json"});
    std::mt19937 rng(20261108);
    struct Entry {
        FzItem* obj;
        std::string mem;   // text in memory
        bool on_disk = false;
        std::string disk;  // text in the file
    };
    std::map<std::int64_t, Entry> entries;
    std::vector<std::int64_t> creation_order;
    std::vector<std::int64_t> file_order;

    auto rand_text = [&](std::size_t len) {
        static const char* alphabet[] = {"a", "b", "c", "x", "y", " ", "7", "0", "\xed\x99\x8d", "\xea\xb8\xb8"};
        std::string s;
        for (std::size_t i = 0; i < len; ++i) s += alphabet[rng() % 10];
        return s;
    };
    auto line_of = [](std::int64_t id, const std::string& text) {
        return "{\"id\": " + std::to_string(id) + ", \"text\": \"" + text + "\", \"f\": 0, \"d\": \"1970-01-01\"}";
    };
    auto expect_file = [&](int step) {
        std::vector<std::string> expected;
        for (std::int64_t id : file_order) expected.push_back(line_of(id, entries[id].disk));
        if (lines_of("Fuzz.json") != expected) {
            std::cerr << "fuzz: file differs from the model after step " << step << '\n';
            ++failures;
            return false;
        }
        return true;
    };
    auto save_one = [&](std::int64_t id) {
        Entry& e = entries[id];
        e.obj->save();
        if (!e.on_disk) {
            e.on_disk = true;
            file_order.push_back(id);
        }
        e.disk = e.mem;
    };
    auto save_all = [&]() {
        DBManager<FzItem>::save();
        for (std::int64_t id : creation_order) {
            Entry& e = entries[id];
            if (!e.on_disk) {
                e.on_disk = true;
                file_order.push_back(id);
            }
            e.disk = e.mem;
        }
    };
    auto change_text = [&](Entry& e, bool same_length) {
        if (same_length) {
            // replace characters by ASCII ones of the same byte length where possible
            std::string s = e.mem;
            for (char& c : s)
                if (c == 'a' || c == 'b' || c == 'c' || c == 'x' || c == 'y') c = "abcxy"[rng() % 5];
            e.mem = s;
        } else {
            e.mem = rand_text(rng() % 30);
        }
        e.obj->text_ = e.mem;
    };

    for (int step = 0; step < 2500; ++step) {
        const int op = static_cast<int>(rng() % 12);
        if (op < 3 || entries.empty()) {  // create
            std::int64_t id;
            do { id = static_cast<std::int64_t>(rng() % 100000); } while (entries.count(id));
            Entry e;
            e.obj = DBManager<FzItem>::create();
            e.obj->id_ = id;
            e.mem = rand_text(rng() % 25);
            e.obj->text_ = e.mem;
            entries[id] = e;
            creation_order.push_back(id);
            if (rng() % 2) save_one(id);
        } else if (op < 8) {  // change one object, maybe save it
            auto it = entries.begin();
            std::advance(it, static_cast<long>(rng() % entries.size()));
            change_text(it->second, rng() % 3 == 0);
            if (rng() % 3 != 0) save_one(it->first);
        } else if (op == 8) {  // change many objects (same length) and bulk save: patches or one rewrite
            const std::size_t count = 1 + rng() % 60;
            for (std::size_t i = 0; i < count; ++i) {
                auto it = entries.begin();
                std::advance(it, static_cast<long>(rng() % entries.size()));
                change_text(it->second, true);
            }
            save_all();
        } else if (op == 9) {  // change many objects (any length) and bulk save
            const std::size_t count = 1 + rng() % 10;
            for (std::size_t i = 0; i < count; ++i) {
                auto it = entries.begin();
                std::advance(it, static_cast<long>(rng() % entries.size()));
                change_text(it->second, false);
            }
            save_all();
        } else {  // bulk save
            save_all();
        }
        if (!expect_file(step)) return;
    }
    CHECK(jq_valid("Fuzz.json"));
    // and nothing is lost when everything is read back by a second table
    const std::size_t n = file_order.size();
    CHECK(DBManager<FzItem>::search("id", GREATER_THAN_OR_EQUAL_TO(-1)).size() == entries.size());
    (void)n;
}

int main() {
    test_missing_and_empty_files();
    test_value_formats_and_round_trip();
    test_parsing_files_written_by_others();
    test_saving_semantics();
    test_duplicate_keys();
    test_get_next_key();
    test_foreign_keys();
    test_mutual_references();
    test_advanced_search();
    test_memory_values_in_search();
    test_search_of_unsaved_objects();
    test_output_operators();
    test_junk_in_files_and_io_errors();
    test_fuzz_file_layout();

    if (failures == 0) std::cout << "All edge-case checks passed\n";
    else std::cout << failures << " check(s) failed\n";
    return failures == 0 ? 0 : 1;
}
