#pragma once

// A small test harness for the host tests: no library to fetch, enough for what the tests need.
//
//   TEST(name) { CHECK(a == b); CHECK_STR(text, "expected"); REQUIRE(ok); }
//
// CHECK records a failure and goes on, REQUIRE returns from the test. The runner prints every
// failure with file and line and exits with 1 when one test failed.

#include <cstring>
#include <string>

namespace hc::test {

using Function = void (*)();

struct Registration {
    Registration(const char *name, Function function);
};

void fail(const char *file, int line, const std::string &what);

// a function, so temporaries of the arguments (a std::string's c_str()) live until the comparison
void check_str(const char *file, int line, const char *expression, const char *actual, const char *expected);

inline void check_str(const char *file, int line, const char *expression, const std::string &actual,
                      const char *expected)
{
    check_str(file, line, expression, actual.c_str(), expected);
}

} // namespace hc::test

#define TEST(name)                                                                \
    static void name();                                                           \
    static const ::hc::test::Registration name##_registration(#name, name);       \
    static void name()

#define CHECK(expression)                                                         \
    do {                                                                          \
        if (!(expression)) {                                                      \
            ::hc::test::fail(__FILE__, __LINE__, #expression);                     \
        }                                                                         \
    } while (0)

#define REQUIRE(expression)                                                       \
    do {                                                                          \
        if (!(expression)) {                                                      \
            ::hc::test::fail(__FILE__, __LINE__, #expression);                     \
            return;                                                               \
        }                                                                         \
    } while (0)

#define CHECK_STR(actual, expected) ::hc::test::check_str(__FILE__, __LINE__, #actual, (actual), (expected))
