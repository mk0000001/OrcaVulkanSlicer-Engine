#pragma once

#include <array>
#include <cstdint>
#include <limits>

#include <boost/multiprecision/cpp_int.hpp>

namespace Slic3r::Gpu {

// GPU kernels use signed 64-bit values. Geometry is first translated into a
// small tile, which keeps all intermediate cross products within that range.
// The host reference deliberately uses a wider type so dispatch validation
// cannot silently accept an overflowing GPU tile.
using Coord = int64_t;
using WideCoord = boost::multiprecision::int128_t;

constexpr Coord coordinate_scale = 1'000'000; // one million fixed-point units per millimetre.
constexpr Coord max_tile_radius = 2'000'000;  // 2 mm from a tile origin.

struct Point {
    Coord x { 0 };
    Coord y { 0 };
};

struct Segment {
    Point a;
    Point b;
};

inline WideCoord orient2d(const Point& a, const Point& b, const Point& c)
{
    const WideCoord abx = WideCoord(b.x) - a.x;
    const WideCoord aby = WideCoord(b.y) - a.y;
    const WideCoord acx = WideCoord(c.x) - a.x;
    const WideCoord acy = WideCoord(c.y) - a.y;
    return abx * acy - aby * acx;
}

inline bool fits_in_tile(const Point& point, const Point& tile_origin)
{
    const WideCoord dx = WideCoord(point.x) - tile_origin.x;
    const WideCoord dy = WideCoord(point.y) - tile_origin.y;
    return dx >= -max_tile_radius && dx <= max_tile_radius &&
           dy >= -max_tile_radius && dy <= max_tile_radius;
}

inline bool fits_in_tile(const Segment& segment, const Point& tile_origin)
{
    return fits_in_tile(segment.a, tile_origin) && fits_in_tile(segment.b, tile_origin);
}

// Stable IDs, rather than GPU workgroup order, are the tie-breaker for every
// wall or infill segment emitted by a compute stage.
struct StableSegment {
    Segment segment;
    uint64_t stable_id { 0 };
};

} // namespace Slic3r::Gpu
