// Tiny self-contained test harness (no third-party download required).
// Each test file defines TESTs; the runner executes all of them and returns
// non-zero if any CHECK failed, which CTest reports as a failure.
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace kt {
struct Test {
    const char* name;
    std::function<void()> fn;
};
inline std::vector<Test>& registry() {
    static std::vector<Test> r;
    return r;
}
inline int& failures() {
    static int f = 0;
    return f;
}
struct Registrar {
    Registrar(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};
}  // namespace kt

#define KT_CAT2(a, b) a##b
#define KT_CAT(a, b) KT_CAT2(a, b)
#define TEST(name)                                                     \
    static void name();                                                \
    static kt::Registrar KT_CAT(reg_, name)(#name, &name);             \
    static void name()

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++kt::failures();                                                             \
        }                                                                                 \
    } while (0)

#define REQUIRE(cond)                                                                     \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "  FAILED %s:%d: %s (aborting test)\n", __FILE__, __LINE__, #cond); \
            ++kt::failures();                                                             \
            return;                                                                       \
        }                                                                                 \
    } while (0)

#define KT_MAIN()                                                                         \
    int main() {                                                                          \
        for (auto& t : kt::registry()) {                                                  \
            const int before = kt::failures();                                            \
            t.fn();                                                                       \
            std::printf("[%s] %s\n", kt::failures() == before ? " OK " : "FAIL", t.name); \
        }                                                                                 \
        std::printf("%zu tests, %d failed checks\n", kt::registry().size(), kt::failures()); \
        return kt::failures() == 0 ? 0 : 1;                                               \
    }
