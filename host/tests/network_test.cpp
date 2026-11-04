// components/network/src/merge.cpp, selection.cpp and idle.cpp: the saved networks after a settings
// import, which saved network is chosen, and the time without activity for modem sleep.

#include <cstdio>
#include <cstring>

#include "check.hpp"
#include "idle.hpp"
#include "merge.hpp"
#include "selection.hpp"

using namespace hc::network;
using namespace hc::network::detail;

namespace {

SavedNetwork saved(const char *ssid, const char *password, bool needed = false)
{
    SavedNetwork n{};
    std::snprintf(n.ssid, sizeof(n.ssid), "%s", ssid);
    std::snprintf(n.password, sizeof(n.password), "%s", password);
    n.password_needed = needed;
    return n;
}

ImportedNetwork imported(const char *ssid, const char *password = nullptr)
{
    ImportedNetwork n{};
    std::snprintf(n.ssid, sizeof(n.ssid), "%s", ssid);
    if (password != nullptr) {
        std::snprintf(n.password, sizeof(n.password), "%s", password);
        n.has_password = true;
    }
    return n;
}

SeenAp ap(const char *ssid, int8_t rssi)
{
    SeenAp a{};
    std::snprintf(a.ssid, sizeof(a.ssid), "%s", ssid);
    a.rssi = rssi;
    a.channel = 1;
    return a;
}

std::string names(const Merged &m)
{
    std::string out;
    for (size_t i = 0; i < m.count; i++) {
        out += (i == 0 ? "" : ",") + std::string(m.networks[i].ssid);
    }
    return out;
}

} // namespace

TEST(import_keeps_saved_passwords_and_takes_the_file_order)
{
    const SavedNetwork old[] = {saved("Home", "home-pass"), saved("Garage", "garage-pass")};
    const ImportedNetwork in[] = {imported("Garage"), imported("Home")};
    Merged m;
    ImportResult r;
    merge(old, 2, -1, in, 2, m, r);
    CHECK_STR(names(m), "Garage,Home");
    CHECK_STR(m.networks[0].password, "garage-pass");
    CHECK_STR(m.networks[1].password, "home-pass");
    CHECK(m.from[0] == 1 && m.from[1] == 0);
    CHECK(r.count == 2 && r.need_password_count == 0 && r.dropped_count == 0);
}

TEST(import_new_networks_need_a_password_unless_the_file_has_one)
{
    const SavedNetwork old[] = {saved("Home", "home-pass")};
    const ImportedNetwork in[] = {imported("Office"), imported("Cafe", ""), imported("Lab", "lab-pass1"),
                                  imported("Home", "new-home-pass")};
    Merged m;
    ImportResult r;
    merge(old, 1, -1, in, 4, m, r);
    CHECK_STR(names(m), "Office,Cafe,Lab,Home");
    CHECK(m.networks[0].password_needed);
    CHECK(!m.networks[1].password_needed); // an empty password in the file: an open network
    CHECK(!m.networks[2].password_needed);
    CHECK_STR(m.networks[2].password, "lab-pass1");
    CHECK_STR(m.networks[3].password, "new-home-pass");
    CHECK(m.from[0] == kNewNetwork && m.from[3] == 0);
    REQUIRE(r.need_password_count == 1);
    CHECK_STR(r.need_password[0], "Office");
}

TEST(import_a_password_clears_the_flag_and_none_keeps_it)
{
    const SavedNetwork old[] = {saved("A", "", true), saved("B", "", true)};
    const ImportedNetwork in[] = {imported("A", "a-password"), imported("B")};
    Merged m;
    ImportResult r;
    merge(old, 2, -1, in, 2, m, r);
    CHECK(!m.networks[0].password_needed);
    CHECK(m.networks[1].password_needed);
    CHECK(r.need_password_count == 1);
}

TEST(import_keeps_saved_networks_the_file_does_not_name)
{
    const SavedNetwork old[] = {saved("Home", "home-pass"), saved("Phone", "phone-pass")};
    const ImportedNetwork in[] = {imported("Office")};
    Merged m;
    ImportResult r;
    merge(old, 2, -1, in, 1, m, r);
    CHECK_STR(names(m), "Office,Home,Phone");
    CHECK(m.from[1] == 0 && m.from[2] == 1);
}

TEST(import_cuts_off_at_five_but_never_the_network_in_use)
{
    const SavedNetwork old[] = {saved("Home", "home-pass"), saved("Phone", "phone-pass")};
    const ImportedNetwork in[] = {imported("A"), imported("B"), imported("C"), imported("D"),
                                  imported("A"), imported(""),  imported("E")};
    Merged m;
    ImportResult r;
    // Phone is in use: Home goes, then E
    merge(old, 2, 1, in, 7, m, r);
    CHECK_STR(names(m), "A,B,C,D,Phone");
    REQUIRE(r.dropped_count == 2);
    CHECK_STR(r.dropped[0], "E");
    CHECK_STR(r.dropped[1], "Home");
    CHECK(r.count == 5 && r.need_password_count == 4);

    merge(old, 2, -1, in, 7, m, r);
    CHECK_STR(names(m), "A,B,C,D,E");
    REQUIRE(r.dropped_count == 2);
    CHECK_STR(r.dropped[0], "Home");
    CHECK_STR(r.dropped[1], "Phone");
}

TEST(import_of_nothing_changes_nothing)
{
    const SavedNetwork old[] = {saved("Home", "home-pass")};
    Merged m;
    ImportResult r;
    merge(old, 1, 0, nullptr, 0, m, r);
    CHECK_STR(names(m), "Home");
    CHECK(m.from[0] == 0 && r.dropped_count == 0);
}

TEST(choose_never_takes_a_network_that_needs_its_password)
{
    const SavedNetwork list[] = {saved("Office", "", true), saved("Home", "home-pass")};
    const SeenAp seen[] = {ap("Office", -40), ap("Home", -60)};
    Candidate c{};
    REQUIRE(choose(list, 2, seen, 2, Policy::EnteredOrder, 0, c));
    CHECK(c.index == 1);
    REQUIRE(choose(list, 2, seen, 2, Policy::StrongestSignal, 0, c));
    CHECK(c.index == 1);
    // Home failed: Office is still not tried
    REQUIRE(choose(list, 2, seen, 2, Policy::EnteredOrder, 1u << 1, c));
    CHECK(c.index == 1);
    CHECK(!choose(list, 1, seen, 2, Policy::EnteredOrder, 0, c));
}

TEST(idle_ticks_counts_up_to_the_delay)
{
    std::atomic<uint32_t> at{1000};
    CHECK(idle_ticks(at, 1000, 300000) == 0);
    CHECK(idle_ticks(at, 61000, 300000) == 60000);
    CHECK(idle_ticks(at, 301000, 300000) == 300000);
    CHECK(at.load() == 1000);
    // a stamp of another task a tick newer than `now`
    CHECK(idle_ticks(at, 999, 300000) == 0);
}

// 60 days at 1 kHz without any activity, checked once a second as the network task does: the
// device stays idle all the time, across 2^31 ticks (24.8 days) and the tick counter's wrap (49.7)
TEST(idle_ticks_stays_idle_for_60_days_without_activity)
{
    const uint32_t delay = 300000;
    std::atomic<uint32_t> at{0};
    uint32_t now = 0;
    bool always_idle = true;
    for (uint64_t second = 300; second <= 60ull * 24 * 3600; second++) {
        now = static_cast<uint32_t>(second * 1000);
        always_idle = always_idle && idle_ticks(at, now, delay) >= delay;
    }
    CHECK(always_idle);
    // an activity after that counts from its stamp again
    at.store(now);
    CHECK(idle_ticks(at, now + 5000, delay) == 5000);
}

// the stamp is not moved over an activity another task wrote: a stamp newer than the delay stays
TEST(idle_ticks_keeps_a_newer_stamp)
{
    std::atomic<uint32_t> at{0xFFFFF000u};
    // across the counter's wrap: 0x2000 ticks after the stamp
    CHECK(idle_ticks(at, 0x1000, 300000) == 0x2000);
    CHECK(at.load() == 0xFFFFF000u);
}
