#include "check.hpp"

#include <cstdio>
#include <vector>

namespace hc::test {

namespace {

struct Entry {
    const char *name;
    Function function;
};

std::vector<Entry> &tests()
{
    static std::vector<Entry> list;
    return list;
}

int failures = 0;

std::string quoted(const char *text)
{
    return text == nullptr ? std::string("nullptr") : "\"" + std::string(text) + "\"";
}

} // namespace

void check_str(const char *file, int line, const char *expression, const char *actual, const char *expected)
{
    if (actual == nullptr || expected == nullptr || std::strcmp(actual, expected) != 0) {
        fail(file, line, std::string(expression) + " is " + quoted(actual) + ", expected " + quoted(expected));
    }
}

Registration::Registration(const char *name, Function function)
{
    tests().push_back({name, function});
}

void fail(const char *file, int line, const std::string &what)
{
    std::printf("  %s(%d): %s\n", file, line, what.c_str());
    failures++;
}

} // namespace hc::test

// hc_tests [part of a test name]: runs every test, or the ones whose name contains the argument
int main(int argc, char **argv)
{
    const char *filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    int failed = 0;
    for (const auto &test : hc::test::tests()) {
        if (filter != nullptr && std::strstr(test.name, filter) == nullptr) {
            continue;
        }
        const int before = hc::test::failures;
        test.function();
        run++;
        if (hc::test::failures != before) {
            std::printf("FAILED %s\n", test.name);
            failed++;
        }
    }
    std::printf("%d tests, %d failed\n", run, failed);
    return failed == 0 ? 0 : 1;
}
