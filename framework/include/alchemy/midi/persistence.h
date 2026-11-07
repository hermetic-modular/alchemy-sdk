#pragma once

#include <algorithm>
#include "alchemy/midi/detail/codec.h"
#include "alchemy/midi/routes.h"
#include "alchemy/surface/serializable.h"

namespace alchemy::midi {

template <size_t Capacity>
class RouteCodec : private detail::Codec {
    static_assert(Capacity > 0 && Capacity <= 65535, "Invalid route capacity");
  public:
    static constexpr uint16_t kVersion = 1;
    static constexpr size_t kRecordSize = 64;
    static constexpr size_t kSize = 16 + Capacity * kRecordSize;
    static constexpr uint32_t kMagic = 0x49444d41u;

    static bool CanEncode(const Routes<Capacity>& routes) {
        Routes<Capacity> canonical;
        for (size_t i = 0; i < routes.Size(); ++i) {
            Route route = *routes.At(i);
            route.session = 0;
            if (!canonical.Add(route)) return false;
        }
        return true;
    }

    static bool Encode(const Routes<Capacity>& routes, uint8_t* output, size_t size) {
        if (!output || size != kSize || !CanEncode(routes)) return false;
        std::fill(output, output + size, uint8_t{0});
        Put32(output, kMagic);
        Put16(output + 4, kVersion);
        Put16(output + 6, static_cast<uint16_t>(routes.Size()));
        Put16(output + 8, static_cast<uint16_t>(Capacity));
        Put16(output + 10, static_cast<uint16_t>(kRecordSize));
        for (size_t i = 0; i < routes.Size(); ++i) {
            const auto& route = *routes.At(i);
            uint8_t* record = output + 16 + i * kRecordSize;
            Put32(record, route.id);
            Put32(record + 4, route.target);
            record[8] = static_cast<uint8_t>(route.kind);
            record[9] = static_cast<uint8_t>(route.operation);
            record[10] = static_cast<uint8_t>(route.reduction);
            record[11] = static_cast<uint8_t>(route.encoding);
            record[12] = static_cast<uint8_t>(route.trigger);
            record[13] = static_cast<uint8_t>((route.bipolar ? 1u : 0u) | (route.advanced ? 2u : 0u));
            Put16(record + 14, route.number);
            Put16(record + 16, route.listen.channel_mask);
            Put16(record + 18, route.listen.port);
            Put32(record + 20, 0);
            PutFloat(record + 24, route.minimum);
            PutFloat(record + 28, route.maximum);
            PutFloat(record + 32, route.step);
            Put16(record + 36, route.center);
            record[38] = static_cast<uint8_t>(route.curve);
            record[39] = static_cast<uint8_t>(route.takeover);
            PutFloat(record + 40, route.exponent);
            PutFloat(record + 44, route.pickup_tolerance);
        }
        Put32(output + 12, Crc(output, size, 12));
        return true;
    }

    static bool Decode(const uint8_t* input, size_t size, Routes<Capacity>& routes) {
        if (!input || size != kSize || Get32(input) != kMagic || Get16(input + 4) != kVersion
            || Get16(input + 8) != Capacity || Get16(input + 10) != kRecordSize
            || Get32(input + 12) != Crc(input, size, 12)) return false;
        const auto count = Get16(input + 6);
        if (count > Capacity) return false;
        Routes<Capacity> staged;
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* record = input + 16 + i * kRecordSize;
            if (record[13] & ~uint8_t{3}) return false;
            if (Get32(record + 20)) return false;
            for (size_t j = 48; j < kRecordSize; ++j) if (record[j]) return false;
            Route route;
            route.id = Get32(record);
            route.target = Get32(record + 4);
            route.kind = static_cast<ControllerKind>(record[8]);
            route.operation = static_cast<RouteOp>(record[9]);
            route.reduction = static_cast<Reduction>(record[10]);
            route.encoding = static_cast<RelativeEncoding>(record[11]);
            route.trigger = static_cast<TriggerEdge>(record[12]);
            route.bipolar = record[13] & 1;
            route.advanced = record[13] & 2;
            route.number = Get16(record + 14);
            route.listen.channel_mask = Get16(record + 16);
            route.listen.port = Get16(record + 18);
            route.session = Get32(record + 20);
            route.minimum = GetFloat(record + 24);
            route.maximum = GetFloat(record + 28);
            route.step = GetFloat(record + 32);
            route.center = Get16(record + 36);
            route.curve = static_cast<Curve>(record[38]);
            route.takeover = static_cast<TakeoverPolicy>(record[39]);
            route.exponent = GetFloat(record + 40);
            route.pickup_tolerance = GetFloat(record + 44);
            if (!staged.Add(route)) return false;
        }
        for (size_t i = 16 + count * kRecordSize; i < size; ++i) if (input[i]) return false;
        routes = staged;
        return true;
    }

    static uint32_t SchemaHash() {
        return 0x4d494449u ^ (static_cast<uint32_t>(Capacity) * 0x9e3779b9u)
            ^ (static_cast<uint32_t>(kVersion) << 16) ^ static_cast<uint32_t>(kRecordSize);
    }

};

template <size_t Capacity>
class RoutesSerializable final : public alchemy::Serializable {
  public:
    using Validator = bool (*)(const Routes<Capacity>&, void*);
    explicit RoutesSerializable(Routes<Capacity>& routes, Validator validator = nullptr, void* context = nullptr)
        : routes_(routes), validator_(validator), context_(context) {}
    bool CanSerialize() const {
        return RouteCodec<Capacity>::CanEncode(routes_) && (!validator_ || validator_(routes_, context_));
    }
    size_t SerializedSize() const override { return RouteCodec<Capacity>::kSize; }
    void Serialize(uint8_t* output) const override {
        if (!output) return;
        if (!CanSerialize() || !RouteCodec<Capacity>::Encode(routes_, output, SerializedSize()))
            std::fill(output, output + SerializedSize(), uint8_t{0});
    }
    bool Deserialize(const uint8_t* input) override {
        Routes<Capacity> staged;
        if (!RouteCodec<Capacity>::Decode(input, SerializedSize(), staged)
            || (validator_ && !validator_(staged, context_))) return false;
        routes_ = staged;
        return true;
    }
    uint32_t SchemaHash() const override { return RouteCodec<Capacity>::SchemaHash(); }
  private:
    Routes<Capacity>& routes_;
    Validator validator_;
    void* context_;
};

} // namespace alchemy::midi
