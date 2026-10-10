// components/network/src/merge.cpp and selection.cpp: the saved networks after a settings
// import, and which saved network is chosen.

#include <cstdio>
#include <cstring>

#include "check.hpp"
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
