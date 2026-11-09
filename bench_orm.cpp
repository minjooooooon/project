// Benchmark for simple_orm: tables with 100,000 objects.
//
// Run it in an empty directory (it creates PStudent.json, PStudentBulk.json, ... there):
//
//     bench_orm all        runs the four steps below, each in a fresh process
//
//     bench_orm write      create / save / modify 100,000 objects (obj->save() and DBManager::save())
//     bench_orm read       fresh process: search(key) for every object, advanced search
//     bench_orm read-adv   fresh process: advanced search straight from the file
//     bench_orm load       fresh process: DBManager::load() and a foreign key vector
//
// "read", "read-adv" and "load" need the files written by "write".

#include "simple_orm.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

using namespace std::chrono;
using Clock = steady_clock;

struct PRoom : public DBTableType {
    StringType room_{"room"};
    IntType cap_{"cap"};
    PRoom() : DBTableType(PrimaryKey(&room_), &cap_) {}
    constexpr static std::string_view name() { return "PRoom"; }
};
#define STUDENT(NAME, FILE)                                                          \
    struct NAME : public DBTableType {                                               \
        StringType id_{"id"};                                                        \
        StringType name_{"name"};                                                    \
        FloatType score_{"score"};                                                   \
        DateType born_{"born"};                                                      \
        ForeignKey<PRoom> room_{"room"};                                             \
        NAME() : DBTableType(PrimaryKey(&id_), &name_, &score_, &born_, &room_) {}   \
        constexpr static std::string_view name() { return FILE; }                    \
    };
STUDENT(PStudent, "PStudent")      // saved one by one
STUDENT(PStudentBulk, "PStudentBulk")  // saved with DBManager::save()
struct PCls : public DBTableType {
    IntType id_ = DBManager<PCls>::get_next_key("id");
    StringType title_{"title"};
    ForeignKeyVector<PStudent> students_{"students"};
    PCls() : DBTableType(PrimaryKey(&id_), &title_, &students_) {}
    constexpr static std::string_view name() { return "PCls"; }
};

static double secs(Clock::time_point a) { return duration<double>(Clock::now() - a).count(); }
static std::string key_of(int i) { char b[32]; std::snprintf(b, sizeof b, "2028%06d", i); return b; }

template <class S>
static void fill(S* s, int i) {
    s->id_ = key_of(i);
    s->name_ = "Student number " + std::to_string(i) + " 홍길동";
    s->score_ = (i % 1000) / 10.0 + 0.05;
    s->born_ = std::chrono::year{1990 + i % 20} / std::chrono::month{unsigned(1 + i % 12)} / std::chrono::day{unsigned(1 + i % 28)};
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const int N = 100000;
    const std::string mode = argc > 1 ? argv[1] : "all";
    if (mode == "all") {
        for (const char* m : {"write", "read", "read-adv", "load"}) {
            std::printf("--- %s\n", m);
            if (std::system((std::string("\"") + argv[0] + "\" " + m).c_str()) != 0) return 1;
        }
    } else if (mode == "write") {
        auto t = Clock::now();
        PRoom* room = DBManager<PRoom>::create();
        room->room_ = "B039"; room->cap_ = 120; room->save();
        for (int i = 0; i < N; ++i) { PStudent* s = DBManager<PStudent>::create(); fill(s, i); s->room_ = room; s->save(); }
        std::printf("create + obj->save() x %d             : %.3f s\n", N, secs(t));

        t = Clock::now();
        for (int i = 0; i < N; ++i) { PStudentBulk* s = DBManager<PStudentBulk>::create(); fill(s, i); }
        std::printf("create x %d (unsaved)                 : %.3f s\n", N, secs(t));
        t = Clock::now();
        for (int i = 0; i < N; i += 1) { if (DBManager<PStudentBulk>::search(key_of(i)) == nullptr) std::puts("BUG"); }
        std::printf("search(key) of %d unsaved objects      : %.3f s\n", N, secs(t));
        t = Clock::now();
        DBManager<PStudentBulk>::save();
        std::printf("DBManager::save() of %d new objects    : %.3f s\n", N, secs(t));

        t = Clock::now();
        DBManager<PStudentBulk>::save();
        std::printf("DBManager::save() nothing changed      : %.3f s\n", secs(t));

        t = Clock::now();
        for (int i = 0; i < N; ++i) DBManager<PStudentBulk>::search(key_of(i))->score_ = 50.0 + (i % 5);
        DBManager<PStudentBulk>::save();
        std::printf("modify all (different len) + bulk save : %.3f s\n", secs(t));
        t = Clock::now();
        for (int i = 0; i < N; ++i) DBManager<PStudentBulk>::search(key_of(i))->score_ = 60.0 + (i % 5);
        DBManager<PStudentBulk>::save();
        std::printf("modify all (same len) + bulk save      : %.3f s\n", secs(t));

        t = Clock::now();
        for (int i = 0; i < 300; ++i) { PStudent* s = DBManager<PStudent>::search(key_of(i * 7)); s->name_ = "changed name " + std::to_string(i); s->save(); }
        std::printf("300 single saves, length changes       : %.3f s\n", secs(t));
        t = Clock::now();
        for (int i = 0; i < N; ++i) { PStudent* s = DBManager<PStudent>::search(key_of(i)); s->name_ = "Student number " + std::to_string(i) + " 김철수"; s->save(); }
        std::printf("%d single saves, same length (patch)  : %.3f s\n", N, secs(t));

        PCls* c = DBManager<PCls>::create();
        c->title_ = "OOP";
        for (int i = 0; i < 20000; ++i) c->students_.push_back(DBManager<PStudent>::search(key_of(i)));
        t = Clock::now(); c->save();
        std::printf("save a class with 20000 students       : %.3f s\n", secs(t));
    } else if (mode == "read") {
        auto t = Clock::now();
        long found = 0;
        for (int i = 0; i < N; ++i) { PStudent* s = DBManager<PStudent>::search(key_of(i)); if (s) ++found; }
        std::printf("fresh process: search(key) x %d       : %.3f s (found %ld)\n", N, secs(t), found);
        t = Clock::now();
        std::printf("search(missing key)                    : %p\n", (void*)DBManager<PStudent>::search("nope"));
        auto r = DBManager<PStudent>::search("score", GREATER_THAN(90.0));
        std::printf("advanced search score>90 (in memory)   : %.3f s (%zu hits)\n", secs(t), r.size());
        t = Clock::now();
        auto r2 = DBManager<PStudent>::search("name", AND(GREATER_THAN_OR_EQUAL_TO("Student number 5"), LESS_THAN("Student number 6"))).order_by("id", Ordering::Descending);
        std::printf("advanced search + order_by             : %.3f s (%zu hits)\n", secs(t), r2.size());
        for (int rep = 0; rep < 20; ++rep) DBManager<PStudent>::search("score", LESS_THAN(5.0));
        std::printf("20 more searches                       : %.3f s total\n", secs(t));
        PRoom* room = DBManager<PRoom>::search("B039");
        std::printf("room cap=%lld\n", (long long)room->cap_.get());
    } else if (mode == "read-adv") {  // advanced search straight from the file, nothing loaded before
        auto t = Clock::now();
        auto r = DBManager<PStudentBulk>::search("score", GREATER_THAN(60.0));
        std::printf("fresh process: advanced search %d rows : %.3f s (%zu hits)\n", N, secs(t), r.size());
        t = Clock::now();
        auto r2 = DBManager<PStudentBulk>::search("born", LESS_THAN(std::chrono::year{1995} / std::chrono::January / 1)).order_by("name", Ordering::Ascending);
        std::printf("second search + order_by               : %.3f s (%zu hits)\n", secs(t), r2.size());
        t = Clock::now();
        long fk = 0;
        for (PStudentBulk* s : r2) if (s->room_.get() == nullptr) ++fk;
        std::printf("FK get() x hits (%ld without room)      : %.3f s\n", fk, secs(t));
    } else if (mode == "load") {
        auto t = Clock::now();
        DBManager<PStudent>::load();
        std::printf("fresh process: load() %d rows          : %.3f s\n", N, secs(t));
        PCls* c = DBManager<PCls>::search(1);
        if (c == nullptr) { std::puts("no class"); return 1; }
        t = Clock::now();
        auto v = c->students_.get();
        std::printf("class FK vector get(): %zu students     : %.3f s\n", v.size(), secs(t));
    } else {
        std::fprintf(stderr, "usage: %s [all|write|read|read-adv|load]\n", argv[0]);
        return 2;
    }
    return 0;
}
