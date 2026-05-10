#include "MarketDataPublisher.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace OrderMatcher;

namespace {

std::string uniqueName(const char* suffix) {
    return std::string("md") + std::to_string(static_cast<long long>(::getpid())) +
           "_" + suffix;
}

void writeRawFeed(const std::string& name, uint32_t version, uint32_t entrySize,
                  uint32_t capacity, bool truncateBacking) {
    std::string shm = "/" + name;
    ::shm_unlink(shm.c_str());
    int fd = ::shm_open(shm.c_str(), O_CREAT | O_RDWR, 0600);
    assert(fd >= 0);

    size_t fullSize = sizeof(ShmHeader) + static_cast<size_t>(capacity) * entrySize;
    size_t mappedSize = truncateBacking ? sizeof(ShmHeader) : fullSize;
    assert(::ftruncate(fd, static_cast<off_t>(mappedSize)) == 0);

    void* ptr = ::mmap(nullptr, mappedSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(ptr != MAP_FAILED);

    auto* header = static_cast<ShmHeader*>(ptr);
    header->magic = ShmHeader::MAGIC;
    header->version = version;
    header->entrySize = entrySize;
    header->capacity = capacity;
    header->writeSeq.store(truncateBacking ? 0 : 1, std::memory_order_release);

    if (!truncateBacking) {
        auto* base = static_cast<char*>(ptr) + sizeof(ShmHeader);
        auto* entry = reinterpret_cast<ShmEntry*>(base);
        std::memset(base, 0, entrySize);
        entry->sequence = 0;
        entry->type = ShmEntry::Type::IncrementalUpdate;
        entry->update.action = MarketDataUpdate::Action::Add;
        entry->update.side = Side::Buy;
        entry->update.level.price = toPrice(101.25);
        entry->update.level.totalQuantity = 40;
        entry->update.level.orderCount = 2;
    }

    ::munmap(ptr, mappedSize);
    ::close(fd);
}

void unlinkRawFeed(const std::string& name) {
    std::string shm = "/" + name;
    ::shm_unlink(shm.c_str());
}

void testPublisherSubscriberRoundTrip() {
    auto name = uniqueName("rt");
    MarketDataPublisher pub(name, 8);
    assert(pub.start());

    MarketDataSubscriber sub(name);
    assert(sub.connect());

    MarketDataUpdate update{};
    update.action = MarketDataUpdate::Action::Add;
    update.side = Side::Sell;
    update.level.price = toPrice(99.50);
    update.level.totalQuantity = 100;
    update.level.orderCount = 3;
    pub.publishUpdate(update);

    ShmEntry out{};
    assert(sub.poll(out));
    assert(out.sequence == 0);
    assert(out.type == ShmEntry::Type::IncrementalUpdate);
    assert(out.update.side == Side::Sell);
    assert(out.update.level.price == toPrice(99.50));
    assert(out.update.level.totalQuantity == 100);

    sub.disconnect();
    pub.stop();
}

void testLargerEntryStrideIsReadable() {
    auto name = uniqueName("ls");
    writeRawFeed(name, ShmHeader::VERSION,
                 static_cast<uint32_t>(sizeof(ShmEntry) + 32),
                 /*capacity=*/4, /*truncateBacking=*/false);

    MarketDataSubscriber sub(name);
    assert(sub.connect());

    ShmEntry out{};
    assert(sub.poll(out));
    assert(out.sequence == 0);
    assert(out.update.level.price == toPrice(101.25));
    assert(out.update.level.totalQuantity == 40);

    sub.disconnect();
    unlinkRawFeed(name);
}

void testUndersizedEntryRejected() {
    auto name = uniqueName("ue");
    writeRawFeed(name, ShmHeader::VERSION,
                 static_cast<uint32_t>(sizeof(ShmEntry) - 1),
                 /*capacity=*/4, /*truncateBacking=*/false);

    MarketDataSubscriber sub(name);
    assert(!sub.connect());
    unlinkRawFeed(name);
}

void testTruncatedBackingRejected() {
    auto name = uniqueName("tr");
    writeRawFeed(name, ShmHeader::VERSION,
                 static_cast<uint32_t>(sizeof(ShmEntry)),
                 /*capacity=*/64, /*truncateBacking=*/true);

    MarketDataSubscriber sub(name);
    assert(!sub.connect());
    unlinkRawFeed(name);
}

} // namespace

int main() {
    testPublisherSubscriberRoundTrip();
    testLargerEntryStrideIsReadable();
    testUndersizedEntryRejected();
    testTruncatedBackingRejected();
    std::puts("MarketDataSchemaTest passed");
    return 0;
}
