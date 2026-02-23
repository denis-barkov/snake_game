#pragma once

#include "../chunk_manager.h"
#include "../world.h"

namespace world {

struct ReplicationRequest {
  int camera_x = 0;
  int camera_y = 0;
  bool aoi_enabled = false;
  int aoi_radius = 1;
};

class ReplicationSystem {
 public:
  // Produces a protocol-compatible snapshot shape (same fields), optionally AOI-filtered.
  static WorldSnapshot BuildSnapshot(const WorldSnapshot& source,
                                     const ChunkManager& chunk_manager,
                                     const ReplicationRequest& req);
};

}  // namespace world
