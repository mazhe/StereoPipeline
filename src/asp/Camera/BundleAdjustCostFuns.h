// __BEGIN_LICENSE__
//  Copyright (c) 2009-2013, United States Government as represented by the
//  Administrator of the National Aeronautics and Space Administration. All
//  rights reserved.
//
//  The NGT platform is licensed under the Apache License, Version 2.0 (the
//  "License"); you may not use this file except in compliance with the
//  License. You may obtain a copy of the License at
//  http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
// __END_LICENSE__

#ifndef __ASP_CAMERA_BUNDLE_ADJUST_COST_FUNCTIONS_H__
#define __ASP_CAMERA_BUNDLE_ADJUST_COST_FUNCTIONS_H__

// Ceres cost functions used by bundle_adjust.

#include <vw/Camera/CameraUtilities.h>
#include <vw/Camera/OpticalBarModel.h>
#include <asp/Camera/CsmModel.h>
#include <asp/Camera/BundleAdjustCamera.h>
#include <asp/Core/StereoSettings.h>
#include <asp/Core/Macros.h>
#include <asp/Core/BundleAdjustUtils.h>

// Turn off warnings from eigen
#if defined(__GNUC__) || defined(__GNUG__)
#define LOCAL_GCC_VERSION (__GNUC__ * 10000                    \
                           + __GNUC_MINOR__ * 100              \
                           + __GNUC_PATCHLEVEL__)
#if LOCAL_GCC_VERSION >= 40600
#pragma GCC diagnostic push
#endif
#if LOCAL_GCC_VERSION >= 40202
#pragma GCC diagnostic ignored "-Wunused-local-typedefs"
#endif
#endif

#include <ceres/ceres.h>
#include <ceres/loss_function.h>

#if defined(__GNUC__) || defined(__GNUG__)
#if LOCAL_GCC_VERSION >= 40600
#pragma GCC diagnostic pop
#endif
#undef LOCAL_GCC_VERSION
#endif

typedef vw::PixelMask<vw::Vector<float, 2>> DispPixelT;

/// Simple base class for unpacking Ceres parameter blocks into
///  a camera model which can do point projections.
class CeresBundleModelBase {
public:

  // These are the same for every camera.
  int num_point_params() const { return 3; }
  int num_pose_params () const { return 6; }

  /// This is for all camera parameters other than the pose parameters.
  /// - These can be spread out across multiple parameter blocks.
  virtual int num_intrinsic_params() const = 0;

  int num_params() const {
    return num_point_params() + num_pose_params() + num_intrinsic_params();
  }

  /// Return the number of Ceres input parameter blocks.
  virtual int num_parameter_blocks() const = 0;

  /// Return the size of each parameter block.
  /// - These should sum up to equal num_params.
  /// - The first block is always the point block (3) and
  ///   the second block is always the pose block (6).
  virtual std::vector<int> get_block_sizes() const;

  /// Read in all of the parameters and generate an output pixel observation.
  /// - Throws if the point does not project in to the camera.
  virtual vw::Vector2 evaluate(std::vector<double const*> const param_blocks) const = 0;
  
}; // End class CeresBundleModelBase

/// Simple wrapper for the vw::camera::AdjustedCameraModel class with a 
/// preconfigured underlying camera.  Only uses translation and rotation.
/// - Just vary the six camera adjustment parameters which are all in 
///   a single parameter block.
class AdjustedCameraBundleModel: public CeresBundleModelBase {
public:

  AdjustedCameraBundleModel(boost::shared_ptr<vw::camera::CameraModel> cam):
    m_underlying_camera(cam) {}

  virtual int num_intrinsic_params() const {return 0;}

  /// Return the number of Ceres input parameter blocks.
  /// - (camera), (point)
  virtual int num_parameter_blocks() const {return 2;}

  /// Read in all of the parameters and compute the residuals.
  virtual vw::Vector2 evaluate(std::vector<double const*> const param_blocks) const;

private:

  /// This camera will be adjusted by the input parameters.
  boost::shared_ptr<vw::camera::CameraModel> m_underlying_camera;

}; // End class CeresBundleModelBase

/// "Full service" pinhole model which solves for all desired camera parameters.
/// - If the current run does not want to solve for everything, those parameter
///   blocks should be set as constant so that Ceres does not change them.
class PinholeBundleModel: public CeresBundleModelBase {
public:

  PinholeBundleModel(boost::shared_ptr<vw::camera::PinholeModel> cam)
    : m_underlying_camera(cam) {}

  /// The number of lens distortion parameters.
  int num_dist_params() const {
    vw::Vector<double> lens_params
      = m_underlying_camera->lens_distortion()->distortion_parameters();
    return lens_params.size();
  }

  virtual int num_intrinsic_params() const {
    // Center, focus, and lens distortion
    return asp::NUM_CENTER_PARAMS + asp::NUM_FOCUS_PARAMS + num_dist_params(); 
  }

  /// Return the number of Ceres input parameter blocks.
  /// - (camera), (point), (center), (focus), (lens distortion)
  virtual int num_parameter_blocks() const { return 5; }

  virtual std::vector<int> get_block_sizes() const;

  /// Read in all of the parameters and compute the residuals.
  virtual vw::Vector2 evaluate(std::vector<double const*> const param_blocks) const;

private:

  // TODO: Cache the constructed camera to save time when just the point changes!

  // TODO: Make const
  /// This camera is used for all of the intrinsic values.
  boost::shared_ptr<vw::camera::PinholeModel> m_underlying_camera;

}; // End class PinholeBundleModel

/// "Full service" optical bar model which solves for all desired camera parameters.
/// - If the current run does not want to solve for everything, those parameter
///   blocks should be set as constant so that Ceres does not change them.
class OpticalBarBundleModel: public CeresBundleModelBase {
public:

  OpticalBarBundleModel(boost::shared_ptr<vw::camera::OpticalBarModel> cam)
    : m_underlying_camera(cam) {}

  virtual int num_intrinsic_params() const {
    // Center, focus, and extra optical bar parameters
    return asp::NUM_CENTER_PARAMS + asp::NUM_FOCUS_PARAMS + asp::NUM_OPTICAL_BAR_EXTRA_PARAMS;
  }

  /// Return the number of Ceres input parameter blocks.
  /// - (camera), (point), (center), (focus), (other intrinsic parameters)
  virtual int num_parameter_blocks() const {return 5;}

  virtual std::vector<int> get_block_sizes() const;

  /// Read in all of the parameters and compute the residuals.
  virtual vw::Vector2 evaluate(std::vector<double const*> const param_blocks) const;
  
private:

  // TODO: Cache the constructed camera to save time when just the point changes!

  // TODO: Make const
  /// This camera is used for all of the intrinsic values.
  boost::shared_ptr<vw::camera::OpticalBarModel> m_underlying_camera;

}; // End class OpticalBarBundleModel

/// "Full service" CSM model which solves for all desired camera parameters.
/// - If the current run does not want to solve for everything, those parameter
///   blocks should be set as constant so that Ceres does not change them.
class CsmBundleModel: public CeresBundleModelBase {
public:

  CsmBundleModel(boost::shared_ptr<asp::CsmModel> cam):
   m_underlying_camera(cam) {}

  /// The number of lens distortion parameters.
  int num_dist_params() const {
    return m_underlying_camera->distortion().size();
  }

  virtual int num_intrinsic_params() const {
     // Center, focus, and lens distortion
    return asp::NUM_CENTER_PARAMS + asp::NUM_FOCUS_PARAMS + num_dist_params();
  }

  /// Return the number of Ceres input parameter blocks.
  /// - (camera), (point), (center), (focus), (lens distortion)
  virtual int num_parameter_blocks() const {return 5;}

  virtual std::vector<int> get_block_sizes() const;

  /// Read in all of the parameters and compute the residuals.
  virtual vw::Vector2 evaluate(std::vector<double const*> const param_blocks) const;
  
private:

  // TODO: Cache the constructed camera to save time when just the point changes!

  // TODO: Make const
  /// This camera is used for all of the intrinsic values.
  boost::shared_ptr<asp::CsmModel> m_underlying_camera;

}; // End class CsmBundleModel

//=========================================================================
// Cost functions for Ceres

/// A Ceres cost function. We pass in the observation and the model.
///  The result is the residual, the difference in the observation 
///  and the projection of the point into the camera, normalized by pixel_sigma.
struct BaReprojectionError {
  BaReprojectionError(vw::Vector2 const& observation, vw::Vector2 const& pixel_sigma,
                      boost::shared_ptr<CeresBundleModelBase> camera_wrapper):
    m_observation(observation),
    m_pixel_sigma(pixel_sigma),
    m_num_param_blocks(camera_wrapper->num_parameter_blocks()),
    m_camera_wrapper(camera_wrapper)
    {}

  // Call to work with ceres::DynamicCostFunctions.
  // - Takes array of arrays.
  bool operator()(double const * const * parameters, double * residuals) const;

  // Factory to hide the construction of the CostFunction object from the client code.
  static ceres::CostFunction* Create(vw::Vector2 const& observation,
                                     vw::Vector2 const& pixel_sigma,
                                     boost::shared_ptr<CeresBundleModelBase> camera_wrapper);
  
private:
  vw::Vector2 m_observation; ///< The pixel observation for this camera/point pair.
  vw::Vector2 m_pixel_sigma;
  size_t  m_num_param_blocks;
  boost::shared_ptr<CeresBundleModelBase> m_camera_wrapper; ///< Pointer to the camera model object.

}; // End class BaReprojectionError

/// A ceres cost function. Here we float two pinhole camera's
/// intrinsic and extrinsic parameters. We take as input a reference
/// xyz point and a disparity from left to right image. The
/// error metric is the following: The reference xyz point is projected in the
/// left image. It is mapped via the disparity to the right
/// image. There, the residual error is the difference between that
/// pixel and the pixel obtained by projecting the xyz point
/// straight into the right image.
struct BaDispXyzError {
  BaDispXyzError(double max_disp_error,
                 double reference_terrain_weight,
                 vw::Vector3 const& reference_xyz,
                 vw::ImageViewRef<DispPixelT> const& interp_disp,
                 boost::shared_ptr<CeresBundleModelBase> left_camera_wrapper,
                 boost::shared_ptr<CeresBundleModelBase> right_camera_wrapper,
                 bool solve_intrinsics, // Would like to remove these!
                 asp::IntrinsicOptions intrinsics_opt):
  m_max_disp_error(max_disp_error),
  m_reference_terrain_weight(reference_terrain_weight),
  m_reference_xyz(reference_xyz),
  m_interp_disp (interp_disp),
  m_num_left_param_blocks (left_camera_wrapper->num_parameter_blocks ()),
  m_num_right_param_blocks(right_camera_wrapper->num_parameter_blocks()),
  m_left_camera_wrapper(left_camera_wrapper ),
  m_right_camera_wrapper(right_camera_wrapper),
  m_solve_intrinsics(solve_intrinsics),
  m_intrinsics_opt(intrinsics_opt) {}

  // Adaptor to work with ceres::DynamicCostFunctions.
  bool operator()(double const* const* parameters, double* residuals) const;

  // TODO: Should this logic live somewhere else?
  /// Create the list of residual pointers when solving for intrinsics.
  /// - Extra logic is needed to avoid duplicate pointers.
  static void get_residual_pointers(asp::BAParams &param_storage,
                                    int left_cam_index, int right_cam_index,
                                    bool solve_intrinsics,
                                    asp::IntrinsicOptions const& intrinsics_opt,
                                    std::vector<double*> &residual_ptrs);
  
  void unpack_residual_pointers(double const* const* parameters,
                                std::vector<double const*> & left_param_blocks,
                                std::vector<double const*> & right_param_blocks) const;
  
  // Factory to hide the construction of the CostFunction object from
  // the client code.
  static ceres::CostFunction* Create(
      double max_disp_error, double reference_terrain_weight,
      vw::Vector3 const& reference_xyz, vw::ImageViewRef<DispPixelT> const& interp_disp,
      boost::shared_ptr<CeresBundleModelBase> left_camera_wrapper,
      boost::shared_ptr<CeresBundleModelBase> right_camera_wrapper,
      bool solve_intrinsics, asp::IntrinsicOptions intrinsics_opt);
  
  double m_max_disp_error, m_reference_terrain_weight;
  vw::Vector3 m_reference_xyz;
  vw::ImageViewRef<DispPixelT> const& m_interp_disp;
  size_t m_num_left_param_blocks, m_num_right_param_blocks;
  // TODO: Make constant!
  boost::shared_ptr<CeresBundleModelBase> m_left_camera_wrapper;
  boost::shared_ptr<CeresBundleModelBase> m_right_camera_wrapper;

  // Would like to not have these two!
  bool m_solve_intrinsics;
  asp::IntrinsicOptions m_intrinsics_opt;
};

/// This cost function imposes a rather hard constraint on camera center
/// horizontal and vertical motion. It does so by knowing how many reprojection
/// errors exist for this camera and making this cost function big enough to
/// overcome then when the motion is going out of bounds. The residual here is
/// raised to 4th power and will be squared when added to the cost function.
/// Two residuals are computed, for horizontal and vertical motion.
struct CamUncertaintyError {
  
  CamUncertaintyError(vw::Vector3 const& orig_ctr, double const* orig_adj,
                      vw::Vector2 const& uncertainty, int num_pixel_obs,
                      vw::cartography::Datum const& datum,
                      double camera_position_uncertainty_power);
    
  bool operator()(const double* cam_adj, double* residuals) const;
  
  // Factory to hide the construction of the CostFunction object from
  // the client code.
  static ceres::CostFunction* Create(vw::Vector3 const& orig_ctr, double const* orig_adj,
                      vw::Vector2 const& uncertainty, int num_pixel_obs,
                      vw::cartography::Datum const& datum, 
                      double camera_position_uncertainty_power) {
    // 2 residuals and 3 translation variables. Must add the rotation variables, however,
    // for CERES not to complain. So, get 6.
    // ceres::RIDDERS works better than ceres::CENTRAL for this cost function,
    // especially when the uncertainty is 0.1 m or less.
    return (new ceres::NumericDiffCostFunction<CamUncertaintyError, ceres::RIDDERS, 2, 6>
            (new CamUncertaintyError(orig_ctr, orig_adj, uncertainty, num_pixel_obs, 
                                     datum, camera_position_uncertainty_power)));
  }

  // orig_ctr is the original camera center, orig_cam_ptr is the original
  // adjustment (resulting in the original center). The uncertainty is
  // in meters.
  vw::Vector3 m_orig_ctr;
  vw::Vector3 m_orig_adj;
  vw::Vector2 m_uncertainty;
  int m_num_pixel_obs;
  vw::Matrix3x3 m_EcefToNed;
  double m_camera_position_uncertainty_power;
};

/// A ceres cost function. The residual is the difference between the
/// observed 3D point lon-lat-height, and the current (floating) 3D
/// point lon-lat-height, normalized by sigma. Used only for
/// ground control points. This has the advantage, unlike
/// XYZError, that when the height is not known reliably,
/// but lon-lat is, we can, in the GCP file, assign a bigger
/// sigma to the latter.
struct LLHError {
  LLHError(vw::Vector3 const& observation_xyz, vw::Vector3 const& sigma, 
           vw::cartography::Datum const& datum):
    m_observation_xyz(observation_xyz), m_sigma(sigma), m_datum(datum) {}

  bool operator()(const double* point, double* residuals) const;
  
  // Factory to hide the construction of the CostFunction object from
  // the client code.
  static ceres::CostFunction* Create(vw::Vector3                const& observation_xyz,
                                     vw::Vector3                const& sigma,
                                     vw::cartography::Datum const& datum);

  vw::Vector3 m_observation_xyz;
  vw::Vector3 m_sigma;
  vw::cartography::Datum m_datum;
};


/// A ceres cost function. The residual is the difference between the
/// original camera center and the current (floating) camera center.
/// This cost function prevents the cameras from straying too far from
/// their starting point.
struct CamError {

  CamError(double const* orig_cam, double weight):
    m_orig_cam(DATA_SIZE), m_weight(weight) {
      for (int i=0; i<DATA_SIZE; ++i)
        m_orig_cam[i] = orig_cam[i];
    }

  template <typename T>
  bool operator()(const T* cam_vec, T* residuals) const {

    // Position units are meters. Don't lock the camera down too tightly.
    const double POSITION_WEIGHT = 1e-2; 
    // Rotation units are in radians. 
    const double ROTATION_WEIGHT = 5e1;

    for (size_t p = 0; p < DATA_SIZE/2; p++) {
      residuals[p] = POSITION_WEIGHT*m_weight*(cam_vec[p] - m_orig_cam[p]);
    }
    for (size_t p = DATA_SIZE/2; p < DATA_SIZE; p++) {
      residuals[p] = ROTATION_WEIGHT*m_weight*(cam_vec[p] - m_orig_cam[p]);
    }

    return true;
  }

  // Factory to hide the construction of the CostFunction object from the client code.
  static ceres::CostFunction* Create(const double *const orig_cam, double weight) {
    return (new ceres::AutoDiffCostFunction<CamError, DATA_SIZE, DATA_SIZE>
            (new CamError(orig_cam, weight)));
  }

private:

  // The camera must be represented by a six element array.
  static const int DATA_SIZE = 6;

  std::vector<double> m_orig_cam;
  double m_weight;
};

/// A ceres cost function. The residual is the rotation + translation
/// vector difference, each multiplied by a weight. Hence, a larger
/// rotation weight will result in less rotation change in the final
/// result, etc. This is somewhat different than CamError as there is no
/// penalty here for this cost function going very large, the scaling is
/// different, and there is finer-grained control. 
struct RotTransError {

  RotTransError(double const* orig_cam, double rotation_weight, double translation_weight):
    m_orig_cam(DATA_SIZE), m_rotation_weight(rotation_weight),
    m_translation_weight(translation_weight) {
    for (int i=0; i<DATA_SIZE; ++i)
        m_orig_cam[i] = orig_cam[i];
    }

  template <typename T>
  bool operator()(const T* cam_vec, T* residuals) const {

    for (size_t p = 0; p < DATA_SIZE/2; p++) {
      residuals[p] = m_translation_weight*(cam_vec[p] - m_orig_cam[p]);
    }

    for (size_t p = DATA_SIZE/2; p < DATA_SIZE; p++) {
      residuals[p] = m_rotation_weight*(cam_vec[p] - m_orig_cam[p]);
    }

    return true;
  }

  // Factory to hide the construction of the CostFunction object from
  // the client code.
  static ceres::CostFunction* Create(const double *const orig_cam,
                                     double rotation_weight, double translation_weight) {
    return (new ceres::AutoDiffCostFunction<RotTransError, DATA_SIZE, DATA_SIZE>
            (new RotTransError(orig_cam, rotation_weight, translation_weight)));

  }

private:

  // The camera must be represented by a six element array.
  static const int DATA_SIZE = 6;

  std::vector<double> m_orig_cam;
  double m_rotation_weight, m_translation_weight;
};

/// From the input options select the correct Ceres loss function.
ceres::LossFunction* get_loss_function(std::string const& cost_function, double th);

// Add a ground constraint (GCP or height from DEM)
void addGcpOrDemConstraint(asp::BaBaseOptions const& opt,
                      std::string             const& cost_function_str, 
                      bool use_llh_error,
                      bool fix_gcp_xyz,
                      // Outputs
                      vw::ba::ControlNetwork & cnet,
                      int                    & num_gcp,
                      int                    & num_gcp_or_dem_residuals,
                      asp::BAParams          & param_storage, 
                      ceres::Problem         & problem);

#endif // __ASP_CAMERA_BUNDLE_ADJUST_COST_FUNCTIONS_H__
