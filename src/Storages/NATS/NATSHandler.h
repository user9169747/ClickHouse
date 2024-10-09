#pragma once

#include <uv.h>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <queue>
#include <nats.h>
#include <base/types.h>
#include <Common/Logger.h>

#include <Storages/NATS/NATSConnection.h>
#include <Storages/UVLoop.h>

namespace DB
{

namespace Loop
{
    static const UInt8 INITIALIZED = 0;
    static const UInt8 RUN = 1;
    static const UInt8 STOP = 2;
    static const UInt8 CLOSED = 3;
}

class NATSHandler
{
    using Task = std::function<void ()>;

public:
    explicit NATSHandler(LoggerPtr log_);

    /// Loop for background thread worker.
    void runLoop();
    void stopLoop();

    std::future<NATSConnectionPtr> createConnection(const NATSConfiguration & configuration, std::uint64_t connect_attempts_count);

private:
    /// Execute task on event loop thread
    void post(Task task);

    NATSOptionsPtr createOptions();

    UVLoop loop;
    LoggerPtr log;

    std::atomic<UInt8> loop_state;

    std::mutex tasks_mutex;
    std::queue<Task> tasks;
};

}
