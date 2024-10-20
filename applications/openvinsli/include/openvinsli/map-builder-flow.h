#ifndef OPENVINSLI_MAP_BUILDER_FLOW_H_
#define OPENVINSLI_MAP_BUILDER_FLOW_H_

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <aslam/cameras/ncamera.h>
#include <message-flow/message-flow.h>
#include <online-map-builders/stream-map-builder.h>
#include <sensors/imu.h>
#include <vi-map/vi-map.h>
#include <vio-common/vio-types.h>

#include "openvinsli/flow-topics.h"
#include "openvinsli/vi-map-with-mutex.h"
#include "openvinsli/vio-update-builder.h"

namespace openvinsli {

// Note that the VisualNFrames are not deep-copied and the passed shared-pointer
// is direcly added to the map.
class MapBuilderFlow {
 public:
  MapBuilderFlow(
      const vi_map::SensorManager& sensor_manager,
      const std::string& save_map_folder);
  void attachToMessageFlow(message_flow::MessageFlow* flow);

  void saveMapAndOptionallyOptimize(
      const std::string& path, const bool overwrite_existing_map,
      const bool process_to_localization_map);

 private:
  VIMapWithMutex::Ptr map_with_mutex_;

  // If set then all incoming callbacks that cause operations on the map will be
  // rejected. This is used during shutdown.
  std::atomic<bool> mapping_terminated_;

  VioUpdateBuilder vio_update_builder_;
  online_map_builders::StreamMapBuilder stream_map_builder_;
};

}  // namespace openvinsli

#endif  // OPENVINSLI_MAP_BUILDER_FLOW_H_
