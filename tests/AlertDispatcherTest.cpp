// AlertDispatcherTest — tests for the webhook alerting system.

#include "AlertDispatcher.h"
#include <cassert>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

using namespace OrderMatcher;

static int passed = 0;
#define TEST(name) std::cout << "  " << #name << "..." << std::flush
#define PASS() do { ++passed; std::cout << " PASS\n"; } while(0)

void test_fire_and_deliver() {
    TEST(FireAndDeliver);

    std::mutex mu;
    std::vector<std::pair<std::string, std::string>> delivered;

    AlertDispatcher alerts;
    alerts.addWebhook("http://test.local/webhook", AlertDispatcher::Format::Generic,
                      "", AlertLevel::Info);
    alerts.setDeliveryFunction([&](const std::string& url,
                                   const std::string& payload) -> bool {
        std::lock_guard<std::mutex> lock(mu);
        delivered.push_back({url, payload});
        return true;
    });
    alerts.start();

    alerts.fire(AlertLevel::Critical, "circuit_breaker",
                "AAPL halted: price moved >5%");

    // Wait for background delivery
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    alerts.stop();

    std::lock_guard<std::mutex> lock(mu);
    assert(delivered.size() == 1);
    assert(delivered[0].first == "http://test.local/webhook");
    assert(delivered[0].second.find("CRITICAL") != std::string::npos);
    assert(delivered[0].second.find("circuit_breaker") != std::string::npos);
    assert(delivered[0].second.find("AAPL") != std::string::npos);

    assert(alerts.alertsFired() == 1);
    assert(alerts.alertsDelivered() == 1);
    assert(alerts.alertsFailed() == 0);

    PASS();
}

void test_slack_format() {
    TEST(SlackFormat);

    std::string payload;
    AlertDispatcher alerts;
    alerts.addWebhook("http://hooks.slack.com", AlertDispatcher::Format::Slack,
                      "", AlertLevel::Info);
    alerts.setDeliveryFunction([&](const std::string&,
                                   const std::string& p) -> bool {
        payload = p;
        return true;
    });
    alerts.start();

    alerts.fire(AlertLevel::Warning, "kill_switch", "Participant 42 killed");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    alerts.stop();

    // Slack format: {"text":"[WARNING] kill_switch: Participant 42 killed"}
    assert(payload.find("\"text\"") != std::string::npos);
    assert(payload.find("[WARNING]") != std::string::npos);
    assert(payload.find("kill_switch") != std::string::npos);

    PASS();
}

void test_pagerduty_format() {
    TEST(PagerDutyFormat);

    std::string payload;
    AlertDispatcher alerts;
    alerts.addWebhook("http://events.pagerduty.com",
                      AlertDispatcher::Format::PagerDuty,
                      "routing-key-123", AlertLevel::Info);
    alerts.setDeliveryFunction([&](const std::string&,
                                   const std::string& p) -> bool {
        payload = p;
        return true;
    });
    alerts.start();

    alerts.fire(AlertLevel::Fatal, "engine", "Unrecoverable journal corruption");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    alerts.stop();

    assert(payload.find("routing-key-123") != std::string::npos);
    assert(payload.find("trigger") != std::string::npos);
    assert(payload.find("critical") != std::string::npos);

    PASS();
}

void test_min_level_filter() {
    TEST(MinLevelFilter);

    int deliveryCount = 0;
    AlertDispatcher alerts;
    // Only fire on Critical+
    alerts.addWebhook("http://test.local/critical",
                      AlertDispatcher::Format::Generic,
                      "", AlertLevel::Critical);
    alerts.setDeliveryFunction([&](const std::string&, const std::string&) -> bool {
        ++deliveryCount;
        return true;
    });
    alerts.start();

    alerts.fire(AlertLevel::Info, "test", "info msg");       // filtered
    alerts.fire(AlertLevel::Warning, "test", "warning msg"); // filtered
    alerts.fire(AlertLevel::Critical, "test", "critical msg"); // delivered
    alerts.fire(AlertLevel::Fatal, "test", "fatal msg");     // delivered

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    alerts.stop();

    assert(deliveryCount == 2);
    assert(alerts.alertsFired() == 4);

    PASS();
}

void test_delivery_failure() {
    TEST(DeliveryFailure);

    AlertDispatcher alerts;
    alerts.addWebhook("http://test.local/fail", AlertDispatcher::Format::Generic,
                      "", AlertLevel::Info);
    alerts.setDeliveryFunction([](const std::string&, const std::string&) -> bool {
        return false;  // simulate failure
    });
    alerts.start();

    alerts.fire(AlertLevel::Critical, "test", "fail msg");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    alerts.stop();

    assert(alerts.alertsFired() == 1);
    assert(alerts.alertsFailed() == 1);
    assert(alerts.alertsDelivered() == 0);

    PASS();
}

void test_multiple_webhooks() {
    TEST(MultipleWebhooks);

    int count = 0;
    AlertDispatcher alerts;
    alerts.addWebhook("http://hook1.local", AlertDispatcher::Format::Generic,
                      "", AlertLevel::Info);
    alerts.addWebhook("http://hook2.local", AlertDispatcher::Format::Slack,
                      "", AlertLevel::Info);
    alerts.setDeliveryFunction([&](const std::string&, const std::string&) -> bool {
        ++count;
        return true;
    });
    alerts.start();

    alerts.fire(AlertLevel::Warning, "test", "broadcast");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    alerts.stop();

    assert(count == 2);  // Delivered to both webhooks

    PASS();
}

void test_json_escaping() {
    TEST(JsonEscaping);

    std::string payload;
    AlertDispatcher alerts;
    alerts.addWebhook("http://test.local", AlertDispatcher::Format::Generic,
                      "", AlertLevel::Info);
    alerts.setDeliveryFunction([&](const std::string&, const std::string& p) -> bool {
        payload = p;
        return true;
    });
    alerts.start();

    alerts.fire(AlertLevel::Info, "test", "msg with \"quotes\" and \\backslash");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    alerts.stop();

    // Quotes should be escaped
    assert(payload.find("\\\"quotes\\\"") != std::string::npos);
    assert(payload.find("\\\\backslash") != std::string::npos);

    PASS();
}

int main() {
    std::cout << "\n═══ AlertDispatcher Tests ═══\n\n";

    test_fire_and_deliver();
    test_slack_format();
    test_pagerduty_format();
    test_min_level_filter();
    test_delivery_failure();
    test_multiple_webhooks();
    test_json_escaping();

    std::cout << "\n─── Results: " << passed << " passed ───\n";
    std::cout << "\nAll alert dispatcher tests passed.\n\n";
    return 0;
}
