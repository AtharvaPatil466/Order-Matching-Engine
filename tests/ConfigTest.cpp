// ConfigTest — tests for the key=value config system.

#include "Config.h"
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>

using namespace OrderMatcher;

static int passed = 0;
#define TEST(name) std::cout << "  " << #name << "..." << std::flush
#define PASS() do { ++passed; std::cout << " PASS\n"; } while(0)

void test_load_file() {
    TEST(LoadFile);

    // Write a temp config file
    const char* path = "/tmp/ob_config_test.conf";
    {
        std::ofstream out(path);
        out << "# Comment\n";
        out << "threads = 8\n";
        out << "journal_path = /var/lib/engine/journal\n";
        out << "lean_mode = true\n";
        out << "max_order_size = 1000000\n";
        out << "price_band = 0.05\n";
        out << "name = \"Order Engine\"\n";
        out << "\n";
        out << "  # indented comment\n";
        out << "  spaced_key  =  spaced_value  \n";
    }

    Config cfg;
    assert(cfg.loadFile(path));

    assert(cfg.getInt("threads") == 8);
    assert(cfg.getString("journal_path") == "/var/lib/engine/journal");
    assert(cfg.getBool("lean_mode") == true);
    assert(cfg.getInt64("max_order_size") == 1000000);
    assert(cfg.getDouble("price_band") == 0.05);
    assert(cfg.getString("name") == "Order Engine");
    assert(cfg.getString("spaced_key") == "spaced_value");

    std::remove(path);
    PASS();
}

void test_defaults() {
    TEST(Defaults);

    Config cfg;
    assert(cfg.getInt("nonexistent", 42) == 42);
    assert(cfg.getString("nonexistent", "default") == "default");
    assert(cfg.getBool("nonexistent", true) == true);
    assert(cfg.getDouble("nonexistent", 3.14) == 3.14);
    assert(!cfg.has("nonexistent"));

    PASS();
}

void test_env_override() {
    TEST(EnvOverride);

    Config cfg;
    cfg.set("threads", "4");
    assert(cfg.getInt("threads") == 4);

    // Set env var OB_THREADS — should override
    setenv("OB_THREADS", "16", 1);
    assert(cfg.getInt("threads") == 16);

    unsetenv("OB_THREADS");
    assert(cfg.getInt("threads") == 4);

    PASS();
}

void test_load_map() {
    TEST(LoadMap);

    Config cfg;
    cfg.loadMap({{"a", "1"}, {"b", "hello"}, {"c", "true"}});

    assert(cfg.getInt("a") == 1);
    assert(cfg.getString("b") == "hello");
    assert(cfg.getBool("c") == true);
    assert(cfg.has("a"));

    PASS();
}

void test_bool_variants() {
    TEST(BoolVariants);

    Config cfg;
    cfg.loadMap({
        {"a", "true"}, {"b", "TRUE"}, {"c", "1"},
        {"d", "yes"}, {"e", "on"}, {"f", "false"},
        {"g", "0"}, {"h", "no"}, {"i", "off"}
    });

    assert(cfg.getBool("a") == true);
    assert(cfg.getBool("b") == true);
    assert(cfg.getBool("c") == true);
    assert(cfg.getBool("d") == true);
    assert(cfg.getBool("e") == true);
    assert(cfg.getBool("f") == false);
    assert(cfg.getBool("g") == false);
    assert(cfg.getBool("h") == false);
    assert(cfg.getBool("i") == false);

    PASS();
}

void test_set_and_dump() {
    TEST(SetAndDump);

    Config cfg;
    cfg.set("x", "42");
    cfg.set("y", "hello");
    assert(cfg.getInt("x") == 42);

    std::string dump = cfg.dump();
    assert(dump.find("x=42") != std::string::npos);
    assert(dump.find("y=hello") != std::string::npos);

    PASS();
}

void test_nonexistent_file() {
    TEST(NonexistentFile);

    Config cfg;
    assert(!cfg.loadFile("/tmp/this_file_does_not_exist_12345.conf"));

    PASS();
}

void test_invalid_lines() {
    TEST(InvalidLines);

    const char* path = "/tmp/ob_config_invalid.conf";
    {
        std::ofstream out(path);
        out << "valid_key = valid_value\n";
        out << "no_equals_sign\n";  // should warn but not crash
        out << "another = good\n";
    }

    Config cfg;
    assert(cfg.loadFile(path));
    assert(cfg.getString("valid_key") == "valid_value");
    assert(cfg.getString("another") == "good");

    std::remove(path);
    PASS();
}

int main() {
    std::cout << "\n═══ Config Tests ═══\n\n";

    test_load_file();
    test_defaults();
    test_env_override();
    test_load_map();
    test_bool_variants();
    test_set_and_dump();
    test_nonexistent_file();
    test_invalid_lines();

    std::cout << "\n─── Results: " << passed << " passed ───\n";
    std::cout << "\nAll config tests passed.\n\n";
    return 0;
}
