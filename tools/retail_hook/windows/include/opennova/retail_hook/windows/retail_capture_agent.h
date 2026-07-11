#pragma once

#include <opennova/retail_hook/capture_agent.h>
#include <opennova/retail_hook/validation_session.h>
#include <opennova/retail_hook/windows/hook_start_config.h>
#include <opennova/retail_hook/windows/winsock_capture.h>
#include <parity/parity.h>

#include <memory>
#include <string>

namespace opennova::retail_hook::windows {

class RetailCaptureAgent {
public:
    RetailCaptureAgent(
        const HookStartConfig& config,
        ValidationSession* session);
    ~RetailCaptureAgent();

    RetailCaptureAgent(const RetailCaptureAgent&) = delete;
    RetailCaptureAgent& operator=(const RetailCaptureAgent&) = delete;

    [[nodiscard]] bool start();
    void stop() noexcept;

    [[nodiscard]] IWireCaptureSink& wire_sink() noexcept;
    [[nodiscard]] bool try_capture(parity::Event event) noexcept;
    [[nodiscard]] bool capture_mutation(
        const MutationResult& result) noexcept;
    [[nodiscard]] CaptureAgentStats wire_stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace opennova::retail_hook::windows
