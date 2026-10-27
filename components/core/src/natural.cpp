#include "hc/core/natural.hpp"

namespace hc::core {

namespace {

bool digit(char c)
{
    return c >= '0' && c <= '9';
}

char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

} // namespace

bool natural_less(std::string_view a, std::string_view b)
{
    size_t i = 0;
    size_t j = 0;
    // the first difference in leading zeros decides between numbers of equal value
    int zeros = 0;
    while (i < a.size() && j < b.size()) {
        if (digit(a[i]) && digit(b[j])) {
            const size_t start_a = i;
            const size_t start_b = j;
            while (i < a.size() && a[i] == '0') {
                i++;
            }
            while (j < b.size() && b[j] == '0') {
                j++;
            }
            const size_t value_a = i;
            const size_t value_b = j;
            while (i < a.size() && digit(a[i])) {
                i++;
            }
            while (j < b.size() && digit(b[j])) {
                j++;
            }
            // more digits without the zeros: the larger number
            if (i - value_a != j - value_b) {
                return i - value_a < j - value_b;
            }
            const int order = a.substr(value_a, i - value_a).compare(b.substr(value_b, j - value_b));
            if (order != 0) {
                return order < 0;
            }
            if (zeros == 0 && value_a - start_a != value_b - start_b) {
                zeros = value_a - start_a < value_b - start_b ? -1 : 1;
            }
            continue;
        }
        const char x = lower(a[i]);
        const char y = lower(b[j]);
        if (x != y) {
            return static_cast<unsigned char>(x) < static_cast<unsigned char>(y);
        }
        i++;
        j++;
    }
    if (a.size() - i != b.size() - j) {
        return a.size() - i < b.size() - j;
    }
    return zeros < 0;
}

} // namespace hc::core
