#include "network_enrichment.h"

#include <npwire/ingame_message_catalog.h>
#include <npwire/wire_capture.h>

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace opennova::parity_tool::detail {
namespace {

struct ProducerKey {
    parity::SourceKind source{parity::SourceKind::unknown};
    parity::RunRole role{parity::RunRole::unknown};
    std::string stream_id{};

    [[nodiscard]] auto as_tuple() const {
        return std::tie(source, role, stream_id);
    }

    [[nodiscard]] bool operator<(const ProducerKey& other) const {
        return as_tuple() < other.as_tuple();
    }
};

struct StreamKey {
    ProducerKey producer{};
    std::uint16_t low_port{};
    std::uint16_t high_port{};

    [[nodiscard]] auto as_tuple() const {
        return std::tie(producer.source,
                        producer.role,
                        producer.stream_id,
                        low_port,
                        high_port);
    }

    [[nodiscard]] bool operator<(const StreamKey& other) const {
        return as_tuple() < other.as_tuple();
    }
};

struct DatagramContext {
    std::uint64_t timestamp_ns{};
    parity::DatagramDirection direction{parity::DatagramDirection::inbound};
    parity::NetworkEndpoint source{};
    parity::NetworkEndpoint destination{};
};

struct StreamState {
    opennova::CaptureDecoder decoder{};
    int next_frame_index{};
    std::map<int, DatagramContext> contexts{};
    StreamCoverage coverage{};
};

ProducerKey producer_key(const parity::ProducerIdentity& identity) {
    return {identity.source, identity.role, identity.stream_id};
}

StreamKey stream_key(const parity::NetworkDatagram& datagram) {
    return {
        producer_key(datagram.identity),
        std::min(datagram.source.port, datagram.destination.port),
        std::max(datagram.source.port, datagram.destination.port),
    };
}

const char* coverage_name(opennova::MsgCoverage coverage) {
    switch (coverage) {
    case opennova::MsgCoverage::Decoded:
        return "decoded";
    case opennova::MsgCoverage::PrinterOnly:
        return "printer-only";
    case opennova::MsgCoverage::Unhandled:
        return "unhandled";
    }
    return "uncatalogued";
}

std::string fallback_message_name(char direction, std::uint16_t tag) {
    std::ostringstream stream;
    stream << (direction == 'S' ? "s2c" : "c2s") << "-unknown-0x"
           << std::hex << std::setfill('0') << std::setw(2)
           << static_cast<unsigned>(tag & 0xffU);
    return stream.str();
}

parity::DecodedNetworkEvent decoded_event(
    const opennova::InGameMessage& message,
    const parity::ProducerIdentity& identity,
    const DatagramContext& context) {
    parity::DecodedNetworkEvent event{};
    event.identity = identity;
    event.simulation_tick = 0;
    event.direction = context.direction;
    event.message_id = message.tag;

    const auto* catalog = opennova::lookup_ingame_message(
        message.dir, static_cast<std::uint8_t>(message.tag & 0xffU));
    event.name = catalog != nullptr
                     ? catalog->name
                     : fallback_message_name(message.dir, message.tag);
    event.fields = {
        {"capture.frame_index",
         static_cast<std::uint64_t>(message.frame_index),
         0.0,
         parity::Severity::info},
        {"capture.timestamp_ns",
         context.timestamp_ns,
         0.0,
         parity::Severity::info},
        {"source.address",
         context.source.address,
         0.0,
         parity::Severity::info},
        {"source.port",
         static_cast<std::uint64_t>(context.source.port),
         0.0,
         parity::Severity::info},
        {"destination.address",
         context.destination.address,
         0.0,
         parity::Severity::info},
        {"destination.port",
         static_cast<std::uint64_t>(context.destination.port),
         0.0,
         parity::Severity::info},
        {"session.port",
         static_cast<std::uint64_t>(message.session),
         0.0,
         parity::Severity::info},
        {"settings_update",
         message.settings_update,
         0.0,
         parity::Severity::info},
        {"payload.size",
         static_cast<std::uint64_t>(message.payload.size()),
         0.0,
         parity::Severity::warning},
        {"decode.coverage",
         std::string{catalog != nullptr ? coverage_name(catalog->coverage)
                                        : "uncatalogued"},
         0.0,
         parity::Severity::warning},
    };
    return event;
}

parity::DiagnosticEvent coverage_diagnostic(const StreamCoverage& coverage) {
    parity::DiagnosticEvent diagnostic{};
    diagnostic.identity = coverage.identity;
    diagnostic.severity = coverage.decoded_events == 0 &&
                                  !coverage.typed_events_supplied
                              ? parity::Severity::warning
                              : parity::Severity::info;
    diagnostic.code = "network.decode.coverage";
    diagnostic.message = "raw datagrams and completed semantic messages";
    diagnostic.context = {
        {"ports.low",
         static_cast<std::uint64_t>(coverage.low_port),
         0.0,
         parity::Severity::info},
        {"ports.high",
         static_cast<std::uint64_t>(coverage.high_port),
         0.0,
         parity::Severity::info},
        {"raw_datagrams",
         coverage.raw_datagrams,
         0.0,
         parity::Severity::info},
        {"decoded_network_events",
         coverage.decoded_events,
         0.0,
         parity::Severity::info},
        {"raw_without_completed_message",
         coverage.raw_without_completed_message(),
         0.0,
         parity::Severity::warning},
        {"typed_events_supplied",
         coverage.typed_events_supplied,
         0.0,
         parity::Severity::info},
    };
    return diagnostic;
}

}  // namespace

EnrichmentResult enrich_network_events(parity::Trace trace) {
    std::set<ProducerKey> producers_with_typed_network_events;
    std::uint64_t supplied_decoded_events = 0;
    for (const parity::Event& event : trace.events) {
        if (const auto* decoded =
                std::get_if<parity::DecodedNetworkEvent>(&event)) {
            producers_with_typed_network_events.insert(
                producer_key(decoded->identity));
            ++supplied_decoded_events;
        }
    }

    std::map<StreamKey, StreamState> states;
    std::vector<parity::Event> events;
    events.reserve(trace.events.size());
    for (parity::Event& event : trace.events) {
        events.push_back(std::move(event));
        auto* datagram =
            std::get_if<parity::NetworkDatagram>(&events.back());
        if (datagram == nullptr) {
            continue;
        }

        const StreamKey key = stream_key(*datagram);
        StreamState& state = states[key];
        state.coverage.identity = datagram->identity;
        state.coverage.low_port = key.low_port;
        state.coverage.high_port = key.high_port;
        state.coverage.typed_events_supplied =
            producers_with_typed_network_events.count(key.producer) != 0;
        ++state.coverage.raw_datagrams;
        const int frame_index = ++state.next_frame_index;
        state.contexts.emplace(
            frame_index,
            DatagramContext{datagram->timestamp_ns,
                            datagram->direction,
                            datagram->source,
                            datagram->destination});

        if (producers_with_typed_network_events.count(key.producer) != 0) {
            continue;
        }
        opennova::CaptureDatagram capture{};
        capture.frame_index = frame_index;
        capture.src_port = datagram->source.port;
        capture.dst_port = datagram->destination.port;
        capture.payload = datagram->payload;
        std::vector<opennova::InGameMessage> messages =
            state.decoder.push(capture);
        if (!messages.empty()) {
            ++state.coverage.datagrams_with_completed_message;
        }
        for (const opennova::InGameMessage& message : messages) {
            auto context = state.contexts.find(message.frame_index);
            const DatagramContext& source_context =
                context != state.contexts.end() ? context->second
                                                : state.contexts.at(frame_index);
            events.emplace_back(decoded_event(
                message, datagram->identity, source_context));
            ++state.coverage.decoded_events;
        }
    }

    EnrichmentResult result{};
    result.trace.major_version = trace.major_version;
    result.trace.minor_version = trace.minor_version;
    result.trace.events = std::move(events);
    result.decoded_events = supplied_decoded_events;
    result.streams.reserve(states.size());
    for (auto& [key, state] : states) {
        (void)key;
        result.raw_datagrams += state.coverage.raw_datagrams;
        result.decoded_events += state.coverage.decoded_events;
        result.raw_without_completed_message +=
            state.coverage.raw_without_completed_message();
        result.has_undecodable_stream |=
            state.coverage.raw_datagrams != 0 &&
            state.coverage.decoded_events == 0 &&
            !state.coverage.typed_events_supplied;
        result.trace.events.emplace_back(
            coverage_diagnostic(state.coverage));
        result.streams.push_back(std::move(state.coverage));
    }
    return result;
}

}  // namespace opennova::parity_tool::detail
