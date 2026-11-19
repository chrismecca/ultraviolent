#pragma once

#include <cstdint>

namespace ultraviolent::ip27 {

// Vector PIO request types (LINUX sn0/hubni.h PIOTYPE_*; IP27.adoc "Vector PIO").
inline constexpr unsigned piotype_read = 0;
inline constexpr unsigned piotype_write = 1;
inline constexpr unsigned piotype_exchange = 3;

// What a vector request found at the end of its route.
struct VectorResult {
    enum class Kind : std::uint8_t {
        // The target served the request: a read's or exchange's old value in `value`.
        response,
        // The route ended at an object with no register at the address.
        address_error,
        // The route led nowhere (an unconnected port): no response arrives.
        no_response,
    };
    Kind kind{Kind::no_response};
    std::uint64_t value{};
};

// An object on the NUMAlink network that route-addressed requests reach (MULTINODE.adoc): a
// Hub's network interface or a router. A request carries the hops still to take, one port
// number per nibble from the lowest (observed: the PROM's vector_get and vector_length, IP27.adoc
// "Vector PIO"); a nibble of 0 ends the route at the object it has reached.
class VectorTarget {
  public:
    virtual ~VectorTarget() = default;
    // A request that arrived through this object's `port` (a router's 1-6; a Hub's is 0),
    // with `route` the hops still to take.
    virtual VectorResult vector_request(std::uint64_t route, unsigned port, unsigned type,
                                        std::uint64_t address, std::uint64_t data) = 0;
    // A warm reset sent down the cable arrived at this object's `port` (NI_PORT_RESET
    // PORTRESET at a Hub, MULTINODE.adoc "N2").
    virtual void link_warm_reset(unsigned port) = 0;

  protected:
    VectorTarget() = default;
    VectorTarget(const VectorTarget&) = default;
    VectorTarget& operator=(const VectorTarget&) = default;
};

} // namespace ultraviolent::ip27
