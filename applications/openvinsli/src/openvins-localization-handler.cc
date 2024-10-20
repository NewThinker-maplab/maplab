#include "openvinsli/openvins-localization-handler.h"

#include <limits>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <Eigen/Core>
#include <aslam/common/pose-types.h>
#include <aslam/common/time.h>
#include <gflags/gflags.h>
#include <maplab-common/conversions.h>
#include <maplab-common/fixed-size-queue.h>
#include <maplab-common/geometry.h>
#include <maplab-common/quaternion-math.h>
#include <vio-common/pose-lookup-buffer.h>
#include <vio-common/vio-types.h>

#include "openvinsli/flow-topics.h"
#include "openvinsli/openvins-factory.h"
#include "openvinsli/openvins-maplab-timetranslation.h"

DEFINE_bool(
    openvinsli_use_6dof_localization, true,
    "Localize using 6dof constraints instead of structure constraints."
    "For now, we only implemented 6dof constraints in openvins");
DEFINE_uint64(
    openvinsli_min_num_baseframe_estimates_before_init, 2u,
    "Number of T_G_M measurements to collect before initializing T_G_M.");
DEFINE_double(
    openvinsli_baseframe_init_position_covariance_msq, 20.0,
    "Position covariance of the baseframe initialization [m^2].");
DEFINE_double(
    openvinsli_baseframe_init_rotation_covariance_radsq, 90.0 * kDegToRad,
    "Rotation covariance of the baseframe initialization [rad^2].");

DEFINE_double(
    openvinsli_max_mean_localization_reprojection_error_px, 100.0,
    "If mean reprojection error of the matches exceeds this value, "
    "reinitialize the baseframe.");

//// Gravity check will be done inside openvins.
// DEFINE_double(
//     openvinsli_localization_max_gravity_misalignment_deg, 5.0,
//     "Localization results are rejected if the angle between the gravity"
//     "direction of the odometry and the localization exceeds this value.");

DEFINE_bool(
    openvinsli_use_6dof_localization_for_inactive_cameras, false,
    "OPENVINS is set to always run in monocular mode, but the maplab part of "
    "OPENVINSLI will build a map and localize based on all cameras. If there is a "
    "localization result for the active OPENVINS camera, it will update the "
    "filter using either 2D-3D correspondences (structure constraints) or 6DoF "
    "constraints.  In structure constraints mode (default) it will ignore the "
    "results of the inactive cameras. If this option is enabled however, it "
    "will use the localization results of the inactive camera as 6DoF update "
    "in case the active camera didn't localize at all.");

DEFINE_int32(
    openvinsli_min_number_of_structure_constraints, 5,
    "After OPENVINSLI rejects structure constraints based on their reprojection "
    "error, this is the minimum number of constraints required to accept a "
    "localization.");

namespace openvinsli {

namespace {
bool getReprojectionErrorForGlobalLandmark(
    const Eigen::Vector3d& p_G, const pose::Transformation& T_G_C,
    const aslam::Camera& camera, const Eigen::Vector2d& measurement,
    double* reprojection_error) {
  CHECK_NOTNULL(reprojection_error);

  const Eigen::Vector3d p_C = T_G_C.inverse() * p_G;
  Eigen::Vector2d reprojected_keypoint;
  const aslam::ProjectionResult result =
      camera.project3(p_C, &reprojected_keypoint);

  CHECK_NE(result.getDetailedStatus(), aslam::ProjectionResult::UNINITIALIZED);
  if (result.getDetailedStatus() ==
          aslam::ProjectionResult::POINT_BEHIND_CAMERA ||
      result.getDetailedStatus() ==
          aslam::ProjectionResult::PROJECTION_INVALID) {
    return false;
  }

  *reprojection_error = (reprojected_keypoint - measurement).norm();
  return true;
}
}  // namespace

OpenvinsLocalizationHandler::OpenvinsLocalizationHandler(
    ov_msckf::VioManager* openvins_interface,
    OpenvinsMaplabTimeTranslation* time_translator,
    const aslam::NCamera& camera_calibration,
    const common::BidirectionalMap<size_t, size_t>&
        maplab_to_openvins_cam_indices_mapping)
    : openvins_interface_(CHECK_NOTNULL(openvins_interface)),
      time_translator_(CHECK_NOTNULL(time_translator)),
      // note(jeffrey): for openvins, we initialize localization by collecting
      //                several raw localizations and check their similarity. and this
      //                is done in openvins, we don't extra pre-filter for the raw
      //                localizations. (and we only implement 6dof constraint in openvins).
      // 6dof constraint based localization does not need initialization. (it's done inside openvins)
      localization_state_(
          FLAGS_openvinsli_use_6dof_localization
              ? common::LocalizationState::kLocalized  // in this state we simply invoke processAsUpdate()
                                                       // for the raw localization.
              : common::LocalizationState::kUninitialized),
      T_M_I_buffer_(kBufferPoseHistoryNs, kBufferMaxPropagationNs),
      T_G_M_filter_buffer_(kFilterBaseframeBufferSize),
      T_G_M_loc_buffer_(FLAGS_openvinsli_min_num_baseframe_estimates_before_init),
      camera_calibration_(camera_calibration),
      maplab_to_openvins_cam_indices_mapping_(
          maplab_to_openvins_cam_indices_mapping) {
  if (FLAGS_openvinsli_use_6dof_localization) {
    LOG(INFO) << "Localization mode: 6dof constraints.";
  } else {
    LOG(INFO) << "Localization mode: structure constraints.";
  }
}

void OpenvinsLocalizationHandler::processLocalizationResultInternal(
    const vio::LocalizationResult::ConstPtr& localization_result) {
  CHECK(localization_result);
  switch (localization_state_) {
    // Fall-through intended.
    case common::LocalizationState::kUninitialized:
    case common::LocalizationState::kNotLocalized: {
      // initializeBaseframe() is unnecessary for openvins (6dof-constraint)
      // since the intialzation of localization will be handled inside openvins.
      const bool success = initializeBaseframe(localization_result);
      if (success) {
        LOG(INFO) << "(Re-)initialized the localization baseframe.";
        localization_state_ = common::LocalizationState::kLocalized;
      }
      break;
    }

    case common::LocalizationState::kLocalized: {
      processAsUpdate(localization_result);
      break;
    }
    default:
      LOG(FATAL) << "Unknown localization state: "
                 << static_cast<int>(localization_state_);
      break;
  }
}

void OpenvinsLocalizationHandler::dealWithBufferedLocalizations() {
  int64_t newest_TMI_timestamp_ns;
  if (!T_M_I_buffer_.getNewestTimestampOfAvailablePose(&newest_TMI_timestamp_ns)) {
    return;
  }

  while (1) {
    vio::LocalizationResult::ConstPtr localization_result;
    {
      std::lock_guard<std::mutex> lock(m_localization_buffer_);
      if (localization_buffer_.empty()) {
        return;
      }
      localization_result = localization_buffer_.front();
      if (localization_result->timestamp_ns > newest_TMI_timestamp_ns) {
        return;
      }
      localization_buffer_.pop_front();
    }
    processLocalizationResultInternal(localization_result);
  }
}


void OpenvinsLocalizationHandler::processLocalizationResult(
      const vio::LocalizationResult::ConstPtr& localization_result) {

  // let openvins itself deal with the localization_result.
  openvins_interface_->feed_measurement_localization(
      makeOpenvinsLocalizationData(localization_result));

  // {
  //   std::lock_guard<std::mutex> lock(m_localization_buffer_);
  //   localization_buffer_.push_back(localization_result);
  // }
  // dealWithBufferedLocalizations();
}


ov_core::LocalizationData OpenvinsLocalizationHandler::makeOpenvinsLocalizationData(
    const vio::LocalizationResult::ConstPtr& localization_result) {

  ov_core::LocalizationData raw_loc;
  raw_loc.timestamp =
      time_translator_->convertMaplabToOpenvinsTimestamp(
          localization_result->timestamp_ns);
  // localization_result->T_G_B
  raw_loc.pm = localization_result->T_G_B.getPosition();
  auto q = localization_result->T_G_B.getRotation().toImplementation();
  raw_loc.qm << q.x(), q.y(), q.z(), q.w();
  // raw_loc.qp_cov = localization_result->T_G_B_covariance;
  raw_loc.qp_cov = Eigen::Matrix<double, 6, 6>::Identity();

  const double loc_orientation_uncertainty = 0.04;  // about 2°
  // const double loc_position_uncertainty = 0.3;  // 
  // const double loc_position_uncertainty = 0.5;  // 
  const double loc_position_uncertainty = 0.8;  // 
  double loc_orientation_var = loc_orientation_uncertainty * loc_orientation_uncertainty;
  double loc_position_var = loc_position_uncertainty * loc_position_uncertainty;
  raw_loc.qp_cov.block<3,3>(0,0) = loc_orientation_var * Eigen::Matrix3d::Identity();
  raw_loc.qp_cov.block<3,3>(3,3) = loc_position_var * Eigen::Matrix3d::Identity();

  return raw_loc;
}


// For now, we only support 6dof constraint in openvins and similar initialization process
// has been done inside openvins. So actually we don't need the method initializeBaseframe().
// We just keep the code for reference.
bool OpenvinsLocalizationHandler::initializeBaseframe(
    const vio::LocalizationResult::ConstPtr& localization_result) {
  CHECK(localization_result);

  // Collect a certain number of localizations before performing the actual
  // initialization.
  aslam::Transformation T_M_I;
  switch (T_M_I_buffer_.getPoseAt(localization_result->timestamp_ns, &T_M_I)) {
    case vio_common::PoseLookupBuffer::ResultStatus::kFailedNotYetAvailable:
    // Fall through intended.
    // todo(jeffrey):
    //     Why not waiting for new T_M_Is? We may need a queue to buffer the 
    //     localization_results and retry the interpolation when new T_M_I becomes available.
    case vio_common::PoseLookupBuffer::ResultStatus::kFailedWillNeverSucceed:
      LOG(WARNING) << "Could not get T_M_I for baseframe initialization.";
      return false;
    default:
      break;
  }

  const aslam::Transformation T_G_M_lc_estimate =
      localization_result->T_G_B * T_M_I.inverse();

  T_G_M_loc_buffer_.insert(T_G_M_lc_estimate);
  if (T_G_M_loc_buffer_.size() <
      FLAGS_openvinsli_min_num_baseframe_estimates_before_init) {
    return false;
  }

  // Perform initialization with LSQ estimate of the baseframe transformation
  // in the buffer.
  const int kNumInliersThreshold = std::ceil(
      FLAGS_openvinsli_min_num_baseframe_estimates_before_init *
      kInitializationRansacInlierRatioThreshold);

  int num_inliers = 0;
  std::random_device device;
  const int ransac_seed = device();
  aslam::Transformation T_G_M_lsq;
  common::transformationRansac(
      T_G_M_loc_buffer_.buffer(), kInitializationMaxNumRansacIterations,
      kInitializationRansacOrientationErrorThresholdRadians,
      kInitializationRansacPositionErrorThresholdMeters, ransac_seed,
      &T_G_M_lsq, &num_inliers);
  if (num_inliers < kNumInliersThreshold) {
    VLOG(1) << "Too few localization transformation inliers (" << num_inliers
            << "/" << T_G_M_loc_buffer_.size() << ").";
    return false;
  }

  // // todo(jeffrey):
  // //     consider how to integrate the initial T_G_M_lsq with open_vins if we
  // //     plan to use structure constraint.
  // //
  // openvins_interface_->feed_measurement_localization(
  //     makeOpenvinsLocalizationData(localization_result));

  // // const aslam::Transformation T_M_G_lsq = T_G_M_lsq.inverse();
  // // const Eigen::Vector3d WrWG = T_M_G_lsq.getPosition();
  // // const kindr::RotationQuaternionPD qWG(
  // //     T_M_G_lsq.getRotation().toImplementation());

  // // openvins_interface_->resetLocalizationMapBaseframeAndCovariance(
  // //     WrWG, qWG, FLAGS_openvinsli_baseframe_init_position_covariance_msq,
  // //     FLAGS_openvinsli_baseframe_init_rotation_covariance_radsq);

  return true;
}

double getLocalizationResultGravityDisparityAngleDeg(
    const vio::LocalizationResult::ConstPtr& localization_result,
    const pose::Transformation& T_G_I_vio) {
  const pose::Transformation& T_G_B = localization_result->T_G_B;

  const Eigen::Vector3d gravity_direction_vio =
      T_G_I_vio.getRotation().inverse().rotate(Eigen::Vector3d::UnitZ());
  const Eigen::Vector3d gravity_direction_localization_pnp =
      T_G_B.getRotation().inverse().rotate(Eigen::Vector3d::UnitZ());

  CHECK_NEAR(gravity_direction_vio.squaredNorm(), 1.0, 1e-8);
  CHECK_NEAR(gravity_direction_localization_pnp.squaredNorm(), 1.0, 1e-8);

  const double error_cosine =
      gravity_direction_vio.dot(gravity_direction_localization_pnp);

  double error_angle_degrees = 180.;
  if (error_cosine <= -1) {
    error_angle_degrees = 180.;
  } else if (error_cosine >= 1) {
    error_angle_degrees = 0.;
  } else {
    // Cosine is in the valid range.
    error_angle_degrees = std::acos(error_cosine) * kRadToDeg;
  }

  return error_angle_degrees;
}

bool OpenvinsLocalizationHandler::processAsUpdate(
    const vio::LocalizationResult::ConstPtr& localization_result) {
  CHECK(localization_result != nullptr);

  const double openvins_timestamp_sec =
      time_translator_->convertMaplabToOpenvinsTimestamp(
          localization_result->timestamp_ns);

  aslam::Transformation T_M_I_filter;
  const vio_common::PoseLookupBuffer::ResultStatus result =
      T_M_I_buffer_.getPoseAt(localization_result->timestamp_ns, &T_M_I_filter);

  CHECK(
      result !=
      vio_common::PoseLookupBuffer::ResultStatus::kFailedNotYetAvailable);
  CHECK(
      result !=
      vio_common::PoseLookupBuffer::ResultStatus::kFailedWillNeverSucceed);

  //// Gravity check will be done inside openvins.
  //
  // const double gravity_error_angle_deg =
  //     getLocalizationResultGravityDisparityAngleDeg(
  //         localization_result, T_M_I_filter);
  // if (gravity_error_angle_deg >
  //     FLAGS_openvinsli_localization_max_gravity_misalignment_deg) {
  //   LOG(WARNING) << "The gravity direction of the localization is not "
  //                << "consistent with the VIO estimate. The disparity angle "
  //                << "is " << gravity_error_angle_deg << "deg (threshold: "
  //                << FLAGS_openvinsli_localization_max_gravity_misalignment_deg
  //                << "). Rejected the localization result.";
  //   return false;
  // }

  bool measurement_accepted = true;
  if (FLAGS_openvinsli_use_6dof_localization) {
    openvins_interface_->feed_measurement_localization(
        makeOpenvinsLocalizationData(localization_result));
    measurement_accepted = true;  // always accept here.
  } else {
    // NOT implemented yet for openvins.

    // Check if there are any matches to be processed in the camera frames that
    // are used by OPENVINS for estimation (inactive).
    const size_t num_cameras =
        localization_result->G_landmarks_per_camera.size();
    size_t num_valid_matches = 0u;
    for (size_t maplab_cam_idx = 0u; maplab_cam_idx < num_cameras;
         ++maplab_cam_idx) {
      const size_t* openvins_cam_idx =
          maplab_to_openvins_cam_indices_mapping_.getRight(maplab_cam_idx);
      if (openvins_cam_idx == nullptr) {
        // Camera is inactive in OPENVINS.
        continue;
      }
      num_valid_matches +=
          localization_result->G_landmarks_per_camera[maplab_cam_idx].cols();
    }
    if (num_valid_matches == 0u) {
      // There are no valid localization matches for the cameras used by OPENVINS.
      // Use the localization result of the inactive cameras but integrate them
      // using the 6dof localization mode, since we cannot currently pass 2D-3D
      // correspondences to OPENVINS for a camera it's not using.
      if (FLAGS_openvinsli_use_6dof_localization_for_inactive_cameras) {
        // OPENVINS coordinate frames:
        //  - J: Inertial frame of pose update
        //  - V: Body frame of pose update sensor
        const Eigen::Vector3d JrJV = localization_result->T_G_B.getPosition();
        const kindr::RotationQuaternionPD qJV(
            localization_result->T_G_B.getRotation().toImplementation());
        // todo(jeffrey):
        // measurement_accepted = openvins_interface_->processGroundTruthUpdate(
        //     JrJV, qJV, openvins_timestamp_sec);
        measurement_accepted = false;

        VLOG_IF(1, measurement_accepted)
            << "No localization found for active camera, successfully updated "
            << "OPENVINS using 6DoF constraints based on localization from "
            << "inactive cameras.";

        LOG_IF(
            WARNING, !measurement_accepted && openvins_interface_->getLastOutput(false,false)->status.initialized)
            << "No localization found for active camera, failed to update "
            << "OPENVINS using 6DoF constraints based on localization from "
            << "inactive cameras, because OPENVINS rejected the localization "
            << "update at time = " << localization_result->timestamp_ns
            << "ns. The latency was too large; consider reducing the "
            << "localization rate.";

        return measurement_accepted;
      }
      return false;
    }

    pose::Transformation T_G_M_filter;
    {
      std::lock_guard<std::mutex> lock(m_T_G_M_filter_buffer_);
      // Buffer cannot be empty as we must have received at least one filter
      // update.
      CHECK(!T_G_M_filter_buffer_.buffer().empty());
      T_G_M_filter = T_G_M_filter_buffer_.buffer().back();
    }

    const pose::Transformation T_G_I_filter = T_G_M_filter * T_M_I_filter;

    std::vector<double> lc_reprojection_errors;
    std::vector<double> filter_reprojection_errors;
    const double reprojection_success_rate = getLocalizationReprojectionErrors(
        *localization_result, T_G_I_filter, &lc_reprojection_errors,
        &filter_reprojection_errors);

    double mean_reprojection_error_diff = std::numeric_limits<double>::max();
    const double kMinReprojectionSuccessRate = 0.5;
    const int num_accepted_localization_constraints =
        lc_reprojection_errors.size();
    const bool reprojection_success =
        reprojection_success_rate > kMinReprojectionSuccessRate &&
        num_accepted_localization_constraints >
            FLAGS_openvinsli_min_number_of_structure_constraints;

    if (reprojection_success) {
      const double lc_reproj_mean = aslam::common::mean(
          lc_reprojection_errors.begin(), lc_reprojection_errors.end());
      const double filter_reproj_mean = aslam::common::mean(
          filter_reprojection_errors.begin(), filter_reprojection_errors.end());
      mean_reprojection_error_diff =
          std::abs(filter_reproj_mean - lc_reproj_mean);
      VLOG(3) << "Localization reprojection error [px]: "
              << mean_reprojection_error_diff;
    }

    if (!reprojection_success ||
        mean_reprojection_error_diff >
            FLAGS_openvinsli_max_mean_localization_reprojection_error_px) {
      if (reprojection_success) {
        LOG(WARNING)
            << "Mean reprojection error of localization matches, "
            << mean_reprojection_error_diff
            << ", is larger than the threshold ("
            << FLAGS_openvinsli_max_mean_localization_reprojection_error_px
            << "). Will reset the localization.";
      } else {
        LOG(WARNING)
            << "Most of the localization matches cannot be reprojected into "
            << "the image plane. Will reset the localization.";
      }
    }

    for (size_t maplab_cam_idx = 0u; maplab_cam_idx < num_cameras;
         ++maplab_cam_idx) {
      const size_t* openvins_cam_idx =
          maplab_to_openvins_cam_indices_mapping_.getRight(maplab_cam_idx);

      if (openvins_cam_idx == nullptr) {
        // Skip this localization result, as the camera was marked as inactive.
        continue;
      }
      // todo(jeffrey):
      // measurement_accepted &=
      //     openvins_interface_->processLocalizationLandmarkUpdates(
      //         *openvins_cam_idx,
      //         localization_result
      //             ->keypoint_measurements_per_camera[maplab_cam_idx],
      //         localization_result->G_landmarks_per_camera[maplab_cam_idx],
      //         openvins_timestamp_sec);
      measurement_accepted &= false;
    }
  }

  LOG_IF(WARNING, !measurement_accepted && openvins_interface_->getLastOutput(false,false)->status.initialized)
      << "OPENVINS rejected localization update at time = "
      << localization_result->timestamp_ns << "ns. The latency was too large; "
      << "consider reducing the localization rate.";
  return measurement_accepted;
}

double OpenvinsLocalizationHandler::getLocalizationReprojectionErrors(
    const vio::LocalizationResult& localization_result,
    const aslam::Transformation& T_G_I_filter,
    std::vector<double>* lc_reprojection_errors,
    std::vector<double>* filter_reprojection_errors) {
  CHECK_NOTNULL(lc_reprojection_errors)->clear();
  CHECK_NOTNULL(filter_reprojection_errors)->clear();

  CHECK_EQ(
      localization_result.G_landmarks_per_camera.size(),
      localization_result.keypoint_measurements_per_camera.size());

  int num_matches_processed = 0;

  const int num_cameras = localization_result.G_landmarks_per_camera.size();
  CHECK_EQ(num_cameras, static_cast<int>(camera_calibration_.numCameras()));
  for (int cam_idx = 0; cam_idx < num_cameras; ++cam_idx) {
    CHECK_EQ(
        localization_result.G_landmarks_per_camera[cam_idx].cols(),
        localization_result.keypoint_measurements_per_camera[cam_idx].cols());

    const size_t* openvins_cam_idx =
        maplab_to_openvins_cam_indices_mapping_.getRight(cam_idx);

    const int num_matches =
        localization_result.G_landmarks_per_camera[cam_idx].cols();

    if (num_matches == 0) {
      continue;
    }

    if (openvins_cam_idx == nullptr) {
      // Skip this localization result, as the camera was marked as inactive.
      continue;
    }

    const pose::Transformation& T_C_B = camera_calibration_.get_T_C_B(cam_idx);
    const pose::Transformation T_G_C_filter =
        (T_C_B * T_G_I_filter.inverse()).inverse();
    const pose::Transformation T_G_C_lc =
        (T_C_B * localization_result.T_G_B.inverse()).inverse();

    for (int i = 0; i < num_matches; ++i) {
      const Eigen::Vector2d& keypoint =
          localization_result.keypoint_measurements_per_camera[cam_idx].col(i);
      const Eigen::Vector3d& p_G =
          localization_result.G_landmarks_per_camera[cam_idx].col(i);

      double reproj_error_sq_filter;
      double reproj_error_sq_lc;
      const aslam::Camera& camera = camera_calibration_.getCamera(cam_idx);
      bool projection_successful = getReprojectionErrorForGlobalLandmark(
          p_G, T_G_C_filter, camera, keypoint, &reproj_error_sq_filter);
      if (projection_successful) {
        projection_successful &= getReprojectionErrorForGlobalLandmark(
            p_G, T_G_C_lc, camera, keypoint, &reproj_error_sq_lc);
      }

      ++num_matches_processed;

      if (projection_successful) {
        lc_reprojection_errors->push_back(reproj_error_sq_filter);
        filter_reprojection_errors->push_back(reproj_error_sq_lc);
      }
    }
  }

  CHECK_EQ(lc_reprojection_errors->size(), filter_reprojection_errors->size());
  CHECK_GT(num_matches_processed, 0);
  return static_cast<double>(lc_reprojection_errors->size()) /
         num_matches_processed;
}

bool extractLocalizationFromOpenvinsState(
    const ov_msckf::VioManager::Output& output, aslam::Transformation* T_G_M) {
  CHECK_NOTNULL(T_G_M);
  bool has_T_G_M = output.status.localized;
  if (has_T_G_M) {
    *T_G_M = aslam::Transformation(
        output.status.T_MtoG.block<3,1>(0,3),
        Eigen::Quaterniond(output.status.T_MtoG.block<3,3>(0,0)));
    common::ensurePositiveQuaternion(&T_G_M->getRotation());
  }
  return has_T_G_M;
}

}  //  namespace openvinsli
