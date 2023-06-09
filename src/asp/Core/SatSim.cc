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

// Functions used for the sat_sim.cc tool that are not general enough to put
// somewhere else.

#include <asp/Core/SatSim.h>
#include <asp/Core/CameraTransforms.h>
#include <asp/Core/Common.h>

#include <vw/Cartography/CameraBBox.h>
#include <vw/Geometry/baseUtils.h>
#include <vw/Cartography/CameraBBox.h>
#include <vw/Cartography/GeoTransform.h>

using namespace vw::cartography;
using namespace vw::math;
using namespace vw::geometry;

namespace fs = boost::filesystem;

namespace asp {

// Convert from projected coordinates to ECEF
vw::Vector3 projToEcef(vw::cartography::GeoReference const& georef,
                vw::Vector3                          const& proj) {
  vw::Vector3 llh = georef.point_to_geodetic(proj);
  vw::Vector3 ecef = georef.datum().geodetic_to_cartesian(llh);
  return ecef;
}

// A function that will read a geo-referenced image, its nodata value,
// and the georeference, and will return a PixelMasked image, the nodata
// value, and the georeference.
// TODO(oalexan1): May need to move this to a more general place.
void readGeorefImage(std::string const& image_file, 
  float & nodata_val, vw::cartography::GeoReference & georef,
  vw::ImageViewRef<vw::PixelMask<float>> & masked_image) {

  // Initial value, in case the image has no nodata field
  nodata_val = std::numeric_limits<float>::quiet_NaN();
  if (!vw::read_nodata_val(image_file, nodata_val))
        vw::vw_out() << "Warning: Could not read the nodata value for: "
                      << image_file << "\nUsing: " << nodata_val << ".\n";

    // Read the image
    vw::vw_out() << "Reading: " << image_file << std::endl;
    vw::DiskImageView<float> image(image_file);
    // Create the masked image
    masked_image = vw::create_mask(image, nodata_val);

    // Read the georeference, and throw an exception if it is missing
    bool has_georef = vw::cartography::read_georeference(georef, image_file);
    if (!has_georef)
      vw::vw_throw(vw::ArgumentErr() << "Missing georeference in: "
                                     << image_file << ".\n");
}

// Compute point on trajectory and along and across track normalized vectors in
// ECEF coordinates, given the first and last proj points and a value t giving
// the position along this line
void calcTrajPtAlongAcross(vw::Vector3 const& first_proj,
                           vw::Vector3 const& last_proj,
                           vw::cartography::GeoReference const& dem_georef,
                           double t,
                           double delta,
                           vw::Vector3 const& proj_along,
                           vw::Vector3 const& proj_across,
                           // Outputs
                           vw::Vector3 & P,
                           vw::Vector3 & along,
                           vw::Vector3 & across) {

    P = first_proj * (1.0 - t) + last_proj * t; // traj

    // Use centered diffrerence to compute the along and across track points
    // This achieves higher quality results

    vw::Vector3 L1 = P - delta * proj_along; // along track point
    vw::Vector3 C1 = P - delta * proj_across; // across track point
    vw::Vector3 L2 = P + delta * proj_along; // along track point
    vw::Vector3 C2 = P + delta * proj_across; // across track point

    // Convert to cartesian
    P  = projToEcef(dem_georef, P);
    L1 = projToEcef(dem_georef, L1);
    C1 = projToEcef(dem_georef, C1);
    L2 = projToEcef(dem_georef, L2);
    C2 = projToEcef(dem_georef, C2);

    // Create the along track and across track vectors
    along = L2 - L1;
    across = C2 - C1;

    // Normalize
    along = along/norm_2(along);
    across = across/norm_2(across);
    // Ensure that across is perpendicular to along
    across = across - dot_prod(along, across) * along;
    // Normalize again
    across = across / norm_2(across);
}

// Assemble the cam2world matrix from the along track, across track, and down vectors
// Note how we swap the first two columns and flip one sign. We went the along
// direction to be the camera y direction
void assembleCam2WorldMatrix(vw::Vector3 const& along, 
                             vw::Vector3 const& across, 
                             vw::Vector3 const& down,
                             // Output
                             vw::Matrix3x3 & cam2world) {

  for (int row = 0; row < 3; row++) {
    cam2world(row, 0) = along[row];
    cam2world(row, 1) = across[row];
    cam2world(row, 2) = down[row];
  }
 return;
}

// Return the matrix of rotation in the xy plane
vw::Matrix3x3 rotationXY() {

  vw::Matrix3x3 T;
  // Set all elements to zero
  for (int row = 0; row < 3; row++)
    for (int col = 0; col < 3; col++)
      T(row, col) = 0.0;
  
  T(0, 1) = 1;
  T(1, 0) = -1;
  T(2, 2) = 1;

  return T;
}

// This is used to signify when the algorithm below fails to find a solution
const double g_big_val = 1e+100;

// Given an orbit given by the first and last camera center positions in
// projected coordinates, a real number t describing the position along this
// line, roll, pitch, and yaw for the camera (relative to nadir), find the z
// direction for the camera (camera look), intersect it with the ground, find
// the DEM pixel location, and return the distance from this location to a given
// pixel location.
double demPixelErr(SatSimOptions const& opt,
                   vw::cartography::GeoReference const& dem_georef,
                   vw::ImageViewRef<vw::PixelMask<float>> dem,
                   vw::Vector3 const& first_proj,
                   vw::Vector3 const& last_proj,
                   vw::Vector3 const& proj_along,
                   vw::Vector3 const& proj_across,
                   double t,
                   double delta, // a small number to move along track
                   double roll, double pitch, double yaw,
                   vw::Vector2 const& pixel_loc) { 

    // Calc position along the trajectory and normalized along and across vectors
    // in ECEF
    vw::Vector3 P, along, across;
    calcTrajPtAlongAcross(first_proj, last_proj, dem_georef, t, delta,
                          proj_along, proj_across, 
                          // Outputs
                          P, along, across);

    // Find the z vector as perpendicular to both along and across
    vw::Vector3 down = vw::math::cross_prod(along, across);
    down = down / norm_2(down);

    // The camera to world rotation
    vw::Matrix3x3 cam2world;
    assembleCam2WorldMatrix(along, across, down, cam2world);
    // Apply the roll-pitch-yaw rotation
    vw::Matrix3x3 R = asp::rollPitchYaw(roll, pitch, yaw);
    cam2world = cam2world * R * rotationXY();

    // Ray from camera to ground going through image center
    vw::Vector3 cam_dir = cam2world * vw::Vector3(0, 0, 1);

    // Find the intersection of this ray with the ground
    bool treat_nodata_as_zero = false;
    bool has_intersection = false;
    double max_abs_tol = std::min(opt.dem_height_error_tol, 1e-14);
    double max_rel_tol = max_abs_tol;
    int num_max_iter = 100;
    vw::Vector3 xyz_guess = vw::Vector3(0, 0, 0);
    // TODO(oalexan1): Consider saving the result from here as guess for next time
    vw::Vector3 xyz = vw::cartography::camera_pixel_to_dem_xyz
      (P, cam_dir, dem,
        dem_georef, treat_nodata_as_zero,
        has_intersection, 
        // Below we use a prudent approach. Try to make the solver work
        // hard. It is not clear if this is needed.
        std::min(opt.dem_height_error_tol, 1e-8),
        max_abs_tol, max_rel_tol, 
        num_max_iter, xyz_guess);

    // Convert to llh
    vw::Vector3 llh = dem_georef.datum().cartesian_to_geodetic(xyz);

    // Find pixel location 
    vw::Vector2 pixel_loc2 = dem_georef.lonlat_to_pixel
      (subvector(llh, 0, 2));

    // If the pixel is outside the DEM, return a big value
    if (!vw::bounding_box(dem).contains(pixel_loc2))
      return g_big_val;

    return norm_2(pixel_loc - pixel_loc2);
}

// A model with the error given by demPixelErr(). The variable will be t,
// which will give the position along the trajectory.
class RayDemPixelLMA : public vw::math::LeastSquaresModelBase<RayDemPixelLMA> {

  SatSimOptions const& m_opt;
  vw::cartography::GeoReference const& m_dem_georef;
  vw::ImageViewRef<vw::PixelMask<float>> m_dem;
  vw::Vector3 m_first_proj;
  vw::Vector3 m_last_proj;
  vw::Vector3 m_proj_along;
  vw::Vector3 m_proj_across;
  double m_delta, m_param_scale_factor;
  double m_roll, m_pitch, m_yaw;
  vw::Vector2 m_pixel_loc;

public:
  typedef vw::Vector<double, 1> result_type;
  typedef vw::Vector<double, 1> domain_type;
  typedef vw::Matrix<double>    jacobian_type; ///< Jacobian form. Auto.

  /// Constructor
  RayDemPixelLMA(SatSimOptions const& opt,
                 vw::cartography::GeoReference const& dem_georef,
                 vw::ImageViewRef<vw::PixelMask<float>> dem,
                 vw::Vector3 const& first_proj,
                 vw::Vector3 const& last_proj,
                 vw::Vector3 const& proj_along,
                 vw::Vector3 const& proj_across,
                 double delta, // a small number to move along track
                 double param_scale_factor, // to go from optimizer units to t in [0, 1]
                 double roll, double pitch, double yaw,
                 vw::Vector2 const& pixel_loc):
    m_opt(opt), m_dem_georef(dem_georef), m_dem(dem),
    m_first_proj(first_proj), m_last_proj(last_proj),
    m_proj_along(proj_along), m_proj_across(proj_across),
    m_delta(delta), m_param_scale_factor(param_scale_factor),
    m_roll(roll), m_pitch(pitch), m_yaw(yaw),
    m_pixel_loc(pixel_loc){}

  // Evaluator operator. The goal is described earlier.
  inline result_type operator()(domain_type const& len) const {

    // See note where param_scale_factor is defined.
    //std::cout << "\n";
    //std::cout << "Len is " << len << std::endl;
    double t = len[0] * m_param_scale_factor;
    double err = demPixelErr(m_opt, m_dem_georef, m_dem,
                             m_first_proj, m_last_proj,
                             m_proj_along, m_proj_across,
                             t, m_delta, m_roll, m_pitch, m_yaw,
                             m_pixel_loc);

    result_type result;
    result[0] = err;
    //std::cout.precision(17);
    //std::cout << "t = " << t << ", err = " << err << std::endl;
    return result;
  }
};

// Find the location of camera center along the trajectory, in projected
// coordinates, so that the ray from the camera center to the ground goes
// closest to given ground point.
void findBestProjCamLocation
  (SatSimOptions const& opt,
   vw::cartography::GeoReference const& dem_georef,
   vw::ImageViewRef<vw::PixelMask<float>> dem,
   vw::Vector3 const& first_proj, vw::Vector3 const& last_proj,
   vw::Vector3 const& proj_along, vw::Vector3 const& proj_across,
   double delta, double roll, double pitch, double yaw,
   vw::Vector2 const& pixel_loc,
   // Outputs
   vw::Vector3 & best_proj) {

  // Note(oalexan1): This algorithm had issues with convergence. Let eps = 1e-7.
  // This is used in LevenbergMarquardt.h for numerical differentiation. Need to
  // ensure model(len) and model(len + eps) are sufficiently different. For
  // that, ensure that len and len + eps correspond to points in orbit separated
  // by about 1 meter. That is why, we start with t in [0, 1], which
  // parametrizes the orbital segment between first_proj and last_proj, and
  // parametrize using value len, with t = len * param_scale_factor. 
  double eps = 1e-7;
  vw::Vector3 P1 = projToEcef(dem_georef, first_proj); // t = 0
  vw::Vector3 P2 = projToEcef(dem_georef, last_proj);  // t = 1
  double d = norm_2(P2 - P1);
  if (d < 1.0)
    vw::vw_throw(vw::ArgumentErr() 
      << "Ensure that the input orbit end points are at least 1 m apart.\n");
  double param_scale_factor = 1.0 / (eps * d);
#if 0 
  // Verification that param_scale_factor is correct
  {
    double l1 = 0, l2 = eps;
    double t1 = param_scale_factor * l1; 
    double t2 = param_scale_factor * l2;
    P1 = projToEcef(dem_georef, first_proj * (1.0 - t1) + last_proj * t1);
    P2 = projToEcef(dem_georef, first_proj * (1.0 - t2) + last_proj * t2);
    std::cout << "Param scale factor is " << param_scale_factor << std::endl;
    std::cout << "Distance must be 1 meter: " << norm_2(P1 - P2) << std::endl;
  }
#endif

  // Find a spacing in t that corresponds to 10 meters movement in orbit.
  // We will use this to find a good initial guess.
  double dt = 1e-3;
  double t1 = -dt, t2 = dt;
  P1 = projToEcef(dem_georef, first_proj * (1.0 - t1) + last_proj * t1);
  P2 = projToEcef(dem_georef, first_proj * (1.0 - t2) + last_proj * t2);
  double slope = norm_2(P2 - P1) / (2*dt);
  double spacing = 100.0 / slope;
#if 0 
  // Verification that spacing is correct
  std::cout << "Spacing is " << spacing << std::endl;
  {
    double t1 = 0, t2 = spacing;
    P1 = projToEcef(dem_georef, first_proj * (1.0 - t1) + last_proj * t1);
    P2 = projToEcef(dem_georef, first_proj * (1.0 - t2) + last_proj * t2);
    std::cout << "Distance must be 100 meters: " << norm_2(P2 - P1) << std::endl;
  }
#endif

  // Set up the LMA problem
  RayDemPixelLMA model(opt, dem_georef, dem, first_proj, last_proj,
                       proj_along, proj_across, delta, param_scale_factor,
                       roll, pitch, yaw, pixel_loc);
  int status = -1;
  double max_abs_tol = 1e-14;
  double max_rel_tol = max_abs_tol;
  int num_max_iter = 100;
  vw::Vector<double, 1> observation; 
  observation[0] = 0; // because we want to minimize the error
  vw::Vector<double, 1> len; len[0] = 0; // initial guess 

  // First need to search around for a good initial guess. This is a bug fix.
  // Number of attempts times spacing in m is 1e+8 m, which is 100,000 km. 
  // Enough for any orbit length.
  // std::cout << "Searching for a good initial guess.\n";
  int attempts = int(1e+8);
  double best_val = g_big_val;
  for (int i = 0; i < attempts; i++) {
    
    // Move towards the positive direction then the negative one
    double curr_best_val = best_val;
    for (int j = -1; j <= 1; j += 2) {
      double t = spacing * i * j;
      vw::Vector<double, 1> len2; 
      len2[0] = t / param_scale_factor;
      double val = model(len2)[0];

      if (val < best_val) {
        best_val = val;
        len = len2;
      }
    }
    
    if (curr_best_val == best_val && curr_best_val < g_big_val) {
      // We are not improving anymore, so so stop here, as otherwise
      // we may be going too far.
      break;
    }

  } // end doing attempts

  // Run the optimization with the just-found initial guess
  // std::cout << "Running the solver.\n";
  len = vw::math::levenberg_marquardt(model, len, observation, status, 
      max_abs_tol, max_rel_tol, num_max_iter);

  // Note: The status is ignored here. We will just take whatever the solver
  // outputs, as it may not converge within tolerance. 

#if 0
// Turning this off, as the minimum cost function may be far from zero.
// May need to add some other check here.
  if (std::abs(model(len)[0]) > 1.0) {
    std::cout << "Abs of model value is " << std::abs(model(len)[0]) << std::endl;
    vw::vw_throw(vw::ArgumentErr() << "Error: The solver for finding correct ends of "
      << "orbital segment did not converge to a good solution. Check your DEM, " 
      << "roll, pitch, yaw, and ground path endpoints.\n");
  }
#endif

  // Compute the best location given the just-found position on the segment
  double t = len[0] * param_scale_factor;
  best_proj = first_proj * (1.0 - t) + last_proj * t;
}

// A function to compute orbit length in ECEF given its endpoints in projected
// coordinates. Use 100,000 samples along the orbit. Should be enough.
double calcOrbitLength(vw::Vector3 const& first_proj,
                       vw::Vector3 const& last_proj,
                       vw::cartography::GeoReference const& dem_georef) {

  // Number of samples along the orbit and corresponding segments                     
  // TODO(oalexan1): See if this is slow. May help to calculate orbit length
  // not from the beginning, but between consecutive samples and add these up.
  int num = 100000;  

  // Start of each segment
  vw::Vector3 beg = projToEcef(dem_georef, first_proj);
  // End of each segment
  vw::Vector3 end = beg;
  double orbitLength = 0.0;

  for (int i = 1; i < num; i++) { // note we start at 1

    double t = double(i) / double(num - 1); 
    // Find the projected position of the current point
    vw::Vector3 curr_proj = first_proj + t * (last_proj - first_proj);
    // Find the ECEF position of the current point
    end = projToEcef(dem_georef, curr_proj);

    // Add the length of the segment
    orbitLength += norm_2(end - beg);
    // Move to the next segment
    beg = end;
  }

  return orbitLength;
}

// A function that will take as input the endpoints and will compute the
// satellite trajectory and along track/across track/down directions in ECEF,
// which will give the camera to world rotation matrix.
// The key observation is that the trajectory will be a straight edge in
// projected coordinates so will be computed there first. In some usage
// modes we will adjust the end points of the trajectory along the way.
void calcTrajectory(SatSimOptions & opt,
                    vw::cartography::GeoReference const& dem_georef,
                    vw::ImageViewRef<vw::PixelMask<float>> dem,
                    // Outputs
                    std::vector<vw::Vector3> & trajectory,
                    // the vector of camera to world rotation matrices
                    std::vector<vw::Matrix3x3> & cam2world,
                    std::vector<vw::Matrix3x3> & ref_cam2world) {

  // Convert the first and last camera center positions to projected coordinates
  vw::Vector3 first_proj, last_proj;
  subvector(first_proj, 0, 2) = dem_georef.pixel_to_point
      (vw::math::subvector(opt.first, 0, 2)); // x and y
  first_proj[2] = opt.first[2]; // z
  subvector(last_proj, 0, 2) = dem_georef.pixel_to_point
      (vw::math::subvector(opt.last,  0, 2)); // x and y
  last_proj[2] = opt.last[2]; // z

  // Validate one more time that we have at least two cameras
  if (opt.num_cameras < 2)
    vw::vw_throw(vw::ArgumentErr() << "The number of cameras must be at least 2.\n");

  // Create interpolated DEM with bilinear interpolation with invalid pixel 
  // edge extension
  vw::PixelMask<float> nodata_mask = vw::PixelMask<float>(); // invalid value
  nodata_mask.invalidate();
  auto interp_dem = vw::interpolate(dem, vw::BilinearInterpolation(),
    vw::ValueEdgeExtension<vw::PixelMask<float>>(nodata_mask));

  // Direction along the edge in proj coords (along track direction)
  vw::Vector3 proj_along = last_proj - first_proj;
  
  // Sanity check
  if (proj_along == vw::Vector3())
    vw::vw_throw(vw::ArgumentErr()
      << "The first and last camera positions are the same.\n");
  // Normalize
  proj_along = proj_along / norm_2(proj_along);
  // One more sanity check
  if (std::max(std::abs(proj_along[0]), std::abs(proj_along[1])) < 1e-6)
    vw::vw_throw(vw::ArgumentErr()
      << "It appears that the satellite is aiming for the ground or "
      << "the orbital segment is too short. Correct the orbit end points.\n");

  // Find the across-track direction, parallel to the ground, in projected coords
  vw::Vector3 proj_across = vw::math::cross_prod(proj_along, vw::Vector3(0, 0, 1));
  proj_across = proj_across / norm_2(proj_across);

  // A small number to help convert directions from being in projected space to
  // ECEF (the transform between these is nonlinear). Do not use a small value,
  // as in ECEF these will be large numbers and we may have precision issues.
  // The value 0.01 was tested well.
  double delta = 0.01; // in meters

  bool have_ground_pos = !std::isnan(norm_2(opt.first_ground_pos)) &&  
      !std::isnan(norm_2(opt.last_ground_pos));
  bool have_roll_pitch_yaw = !std::isnan(opt.roll) && !std::isnan(opt.pitch) &&
      !std::isnan(opt.yaw);

  // Starting point of orbit before we adjust it to match the desired
  // ground locations and roll/pitch/yaw angles.
  vw::Vector3 orig_first_proj = first_proj;

  if (have_ground_pos && have_roll_pitch_yaw) {
    // Find best starting and ending points for the orbit given desired
    // ground locations and roll/pitch/yaw angles.
    // Print a message as this step can take a while
    vw::vw_out() << "Estimating orbit endpoints.\n";
    vw::Vector3 first_best_cam_loc_proj;
    findBestProjCamLocation(opt, dem_georef, dem, first_proj, last_proj,
                            proj_along, proj_across, delta, 
                            opt.roll, opt.pitch, opt.yaw,
                            opt.first_ground_pos, first_best_cam_loc_proj);
    // Same thing for the last camera
    vw::Vector3 last_best_cam_loc_proj;
    findBestProjCamLocation(opt, dem_georef, dem, first_proj, last_proj,
                            proj_along, proj_across, delta, 
                            opt.roll, opt.pitch, opt.yaw,
                            opt.last_ground_pos, last_best_cam_loc_proj);
    // Overwrite the first and last camera locations in projected coordinates
    // with the best ones
    first_proj = first_best_cam_loc_proj;
    last_proj  = last_best_cam_loc_proj;
  }                  

  // We did a sanity check to ensure that when opt.jitter_frequency is set,
  // opt.velocity and and opt.horizontal_uncertainty are also set and not NaN.
  bool model_jitter = (!std::isnan(opt.jitter_frequency));

  // Find the trajectory, as well as points in the along track and across track 
  // directions in the projected space
  std::vector<vw::Vector3> along_track(opt.num_cameras), across_track(opt.num_cameras);
  trajectory.resize(opt.num_cameras);
  cam2world.resize(opt.num_cameras);
  ref_cam2world.resize(opt.num_cameras);
  for (int i = 0; i < opt.num_cameras; i++) {
    double t = double(i) / double(opt.num_cameras - 1);

    // Calc position along the trajectory and normalized along and across vectors
    // in ECEF
    vw::Vector3 P, along, across;
    calcTrajPtAlongAcross(first_proj, last_proj, dem_georef, t, delta,
                          proj_along, proj_across, 
                          // Outputs
                          P, along, across);

    if (have_ground_pos && !have_roll_pitch_yaw) {
      // The camera will be constrained by the ground, but not by the roll/pitch/yaw,
      // then the orientation will change along the trajectory.
      vw::Vector2 ground_pix = opt.first_ground_pos * (1.0 - t) + opt.last_ground_pos * t;

      // Find the projected position along the ground path
      vw::Vector3 ground_proj_pos;
      subvector(ground_proj_pos, 0, 2) = dem_georef.pixel_to_point(ground_pix); // x and y
      auto val = interp_dem(ground_pix[0], ground_pix[1]);
      if (!is_valid(val))
        vw::vw_throw(vw::ArgumentErr() 
          << "Could not interpolate into the DEM along the ground path.\n");
      ground_proj_pos[2] = val.child(); // z

      // Convert the ground point to ECEF
      vw::Vector3 G = projToEcef(dem_georef, ground_proj_pos);

      // Find the ground direction
      vw::Vector3 ground_dir = G - P;
      if (norm_2(ground_dir) < 1e-6)
        vw::vw_throw(vw::ArgumentErr()
          << "The ground position is too close to the camera.\n");

      // Normalize      
      along = along / norm_2(along);
      ground_dir = ground_dir / norm_2(ground_dir);

      // Adjust the along-track direction to make it perpendicular to ground dir
      along = along - dot_prod(ground_dir, along) * ground_dir;

      // Find 'across' as y direction, given that 'along' is x, and 'ground_dir' is z
      across = -vw::math::cross_prod(along, ground_dir);
    }

    // Normalize
    along = along / norm_2(along);
    across = across / norm_2(across);
    // Ensure that across is perpendicular to along
    across = across - dot_prod(along, across) * along;
    // Normalize again
    across = across / norm_2(across);

    // Find the z vector as perpendicular to both along and across
    vw::Vector3 down = vw::math::cross_prod(along, across);
    down = down / norm_2(down);

    // Trajectory
    trajectory[i] = P;
    
    // The camera to world rotation has these vectors as the columns
    assembleCam2WorldMatrix(along, across, down, cam2world[i]);

    vw::Vector3 amp(0, 0, 0);
    if (model_jitter) {
      // Model the jitter as a sinusoidal motion in the along-track direction
      // Use a different amplitude for roll, pitch, and yaw.

      // Current postion in projected coordinates and height above datum for it
      vw::Vector3 curr_proj = first_proj * (1.0 - t) + last_proj * t;
      double height_above_datum = curr_proj[2];

      // Length of the orbit from starting point, before adjustment for roll,
      // pitch, and yaw. This way when different orbital segments are used, for
      // different roll, pitch, and yaw, d will not always start as 0 at
      // the beginning of each segment.
      double dist = calcOrbitLength(orig_first_proj, curr_proj, dem_georef);
      double v = opt.velocity;
      double f = opt.jitter_frequency;
      double T = v / f; // period in meters
      
      for (int c = 0; c < 3; c++) {
        // jitter amplitude as angular uncertainty given ground uncertainty
        double a = atan(opt.horizontal_uncertainty[c] / height_above_datum);
        // Covert to degrees
        a = a * 180.0 / M_PI;
        amp[c] = a * sin(dist * 2.0 * M_PI / T);
      }
    }

    // Save this before applying adjustments as below
    ref_cam2world[i] = cam2world[i];

    // if to apply a roll, pitch, yaw rotation
    if (have_roll_pitch_yaw) {
      vw::Matrix3x3 R = asp::rollPitchYaw(opt.roll  + amp[0], 
                                          opt.pitch + amp[1], 
                                          opt.yaw   + amp[2]);
      cam2world[i] = cam2world[i] * R * rotationXY();
    }
  }
  return;
}

// Generate a prefix that will be used for image names and camera names
std::string genPrefix(SatSimOptions const& opt, int i) {
  return opt.out_prefix + "-" + num2str(10000 + i);
}

// Generate a prefix that will be used for reference camera, without 
// roll, pitch, yaw, jitter, or rotation from camera to satellite frame
std::string genRefPrefix(SatSimOptions const& opt, int i) {
  return opt.out_prefix + "-ref-" + num2str(10000 + i);
}

// A function to read the pinhole cameras from disk
void readCameras(SatSimOptions const& opt, 
    std::vector<std::string> & cam_names,
    std::vector<vw::camera::PinholeModel> & cams) {

  // Read the camera names
  vw::vw_out() << "Reading: " << opt.camera_list << std::endl;
  asp::read_list(opt.camera_list, cam_names);

  // Sanity check
  if (cam_names.empty())
    vw::vw_throw(vw::ArgumentErr() << "No cameras were found.\n");

  cams.resize(cam_names.size());
  for (int i = 0; i < int(cam_names.size()); i++)
    cams[i].read(cam_names[i]);
  
  return;
}

// Check if we do a range
bool skipCamera(int i, SatSimOptions const& opt) {

  if (opt.first_index >= 0 && opt.last_index >= 0 &&
     (i < opt.first_index || i >= opt.last_index))
       return true;
  return false;
}

// A function to create and save the cameras. Assume no distortion, and pixel
// pitch = 1.
void genCameras(SatSimOptions const& opt, std::vector<vw::Vector3> const & trajectory,
                std::vector<vw::Matrix3x3> const & cam2world,
                std::vector<vw::Matrix3x3> const & ref_cam2world,
                // outputs
                std::vector<std::string> & cam_names,
                std::vector<vw::camera::PinholeModel> & cams) {

  // Ensure we have as many camera positions as we have camera orientations
  if (trajectory.size() != cam2world.size())
    vw::vw_throw(vw::ArgumentErr()
      << "Expecting as many camera positions as camera orientations.\n");

    cams.resize(trajectory.size());
    cam_names.resize(trajectory.size());
    for (int i = 0; i < int(trajectory.size()); i++) {

      // Always create the cameras, but only save them if we are not skipping
      cams[i] = vw::camera::PinholeModel(trajectory[i], cam2world[i],
                                   opt.focal_length, opt.focal_length,
                                   opt.optical_center[0], opt.optical_center[1]);
      
      // This is useful for understanding things in the satellite frame
      vw::camera::PinholeModel refCam;
      if (opt.save_ref_cams)  
          refCam = vw::camera::PinholeModel(trajectory[i], ref_cam2world[i],
                                   opt.focal_length, opt.focal_length,
                                   opt.optical_center[0], opt.optical_center[1]); 

      std::string camName = genPrefix(opt, i) + ".tsai";
      cam_names[i] = camName;

      // Check if we do a range
      if (skipCamera(i, opt)) continue;

      vw::vw_out() << "Writing: " << camName << std::endl;
      cams[i].write(camName);

      if (opt.save_ref_cams) {
        std::string refCamName = genRefPrefix(opt, i) + ".tsai";
        vw::vw_out() << "Writing: " << refCamName << std::endl;
        refCam.write(refCamName);
      }
    }
  
  return;
}

// Create a synthetic image with multiple threads
typedef vw::ImageView<vw::PixelMask<float>> ImageT;
class SynImageView: public vw::ImageViewBase<SynImageView> {
  
  typedef typename ImageT::pixel_type PixelT;
  SatSimOptions const& m_opt;
  vw::camera::PinholeModel m_cam;
   vw::cartography::GeoReference m_dem_georef; // make a copy to be thread-safe
   vw::ImageView<vw::PixelMask<float>> const& m_dem;
   vw::cartography::GeoReference m_ortho_georef; // make a copy to be thread-safe
   vw::ImageView<vw::PixelMask<float>> const& m_ortho;
   float m_ortho_nodata_val;

public:
  SynImageView(SatSimOptions const& opt,
               vw::camera::PinholeModel        const& cam,
               vw::cartography::GeoReference   const& dem_georef,
               vw::ImageView<vw::PixelMask<float>> dem,
               vw::cartography::GeoReference   const& ortho_georef,
               vw::ImageView<vw::PixelMask<float>> ortho,
               float ortho_nodata_val):
                m_opt(opt), m_cam(cam), 
                m_dem_georef(dem_georef), m_dem(dem),
                m_ortho_georef(ortho_georef), m_ortho(ortho),
                m_ortho_nodata_val(ortho_nodata_val) {}

  typedef PixelT pixel_type;
  typedef PixelT result_type;
  typedef vw::ProceduralPixelAccessor<SynImageView> pixel_accessor;

  inline vw::int32 cols() const { return m_opt.image_size[0]; }
  inline vw::int32 rows() const { return m_opt.image_size[1]; }
  inline vw::int32 planes() const { return 1; }

  inline pixel_accessor origin() const { return pixel_accessor(*this, 0, 0); }

  inline pixel_type operator()( double/*i*/, double/*j*/, vw::int32/*p*/ = 0 ) const {
    vw::vw_throw(vw::NoImplErr() 
      << "SynImageView::operator()(...) is not implemented");
    return pixel_type();
  }

  typedef vw::CropView<vw::ImageView<pixel_type>> prerasterize_type;
  inline prerasterize_type prerasterize(vw::BBox2i const& bbox) const {

  // Create interpolated image with bicubic interpolation with invalid pixel 
  // edge extension
  vw::PixelMask<float> nodata_mask = vw::PixelMask<float>(); // invalid value
  nodata_mask.invalidate();
  auto interp_ortho = vw::interpolate(m_ortho, vw::BicubicInterpolation(),
                                      vw::ValueEdgeExtension<vw::PixelMask<float>>(nodata_mask));

  // The location where the ray intersects the ground. We will use each obtained
  // location as initial guess for the next ray. This may not be always a great
  // guess, but it is better than starting nowhere. It should work decently
  // if the camera is high, and with a small footprint on the ground.
  vw::Vector3 xyz(0, 0, 0);

  vw::ImageView<result_type> tile(bbox.width(), bbox.height());

    for (int col = bbox.min().x(); col < bbox.max().x(); col++) {
      for (int row = bbox.min().y(); row < bbox.max().y(); row++) {

        // These will use to index into the tile 
        int c = col - bbox.min().x();
        int r = row - bbox.min().y();

        // Start with an invalid pixel
        tile(c, r) = vw::PixelMask<float>();
        tile(c, r).invalidate();

        // Here use the full image pixel indices
        vw::Vector2 pix(col, row);

        vw::Vector3 cam_ctr = m_cam.camera_center(pix);
        vw::Vector3 cam_dir = m_cam.pixel_to_vector(pix);

        // Intersect the ray going from the given camera pixel with a DEM
        // Use xyz as initial guess and overwrite it with the new value
        bool treat_nodata_as_zero = false;
        bool has_intersection = false;
        double max_abs_tol = std::min(m_opt.dem_height_error_tol, 1e-14);
        double max_rel_tol = max_abs_tol;
        int num_max_iter = 100;
        xyz = vw::cartography::camera_pixel_to_dem_xyz
          (cam_ctr, cam_dir, m_dem,
            m_dem_georef, treat_nodata_as_zero,
            has_intersection, m_opt.dem_height_error_tol, 
            max_abs_tol, max_rel_tol, 
            num_max_iter, xyz);

        if (!has_intersection) 
          continue; // will result in nodata pixels

        // Find the texture value at the intersection point by interpolation.
        // This will result in an invalid value if if out of range or if the
        // image itself has invalid pixels.
        vw::Vector3 llh = m_dem_georef.datum().cartesian_to_geodetic(xyz);
        vw::Vector2 ortho_pix = m_ortho_georef.lonlat_to_pixel
                                 (vw::Vector2(llh[0], llh[1]));

        tile(c, r) = interp_ortho(ortho_pix[0], ortho_pix[1]);
      }
    }

    return prerasterize_type(tile, -bbox.min().x(), -bbox.min().y(),
                             cols(), rows());
  }

  template <class DestT>
  inline void rasterize(DestT const& dest, vw::BBox2i bbox) const {
    vw::rasterize(prerasterize(bbox), dest, bbox);
  }
};

// Bring crops in memory. It greatly helps with multi-threading speed.  
void setupCroppedDemAndOrtho(vw::Vector2 const& image_size,
  vw::camera::PinholeModel const& cam,
    vw::ImageViewRef<vw::PixelMask<float>> const& dem,
    vw::cartography::GeoReference const& dem_georef,
    vw::ImageViewRef<vw::PixelMask<float>> const& ortho,
    vw::cartography::GeoReference const& ortho_georef,
    // Outputs
    vw::ImageView<vw::PixelMask<float>> & crop_dem,
    vw::cartography::GeoReference & crop_dem_georef,
    vw::ImageView<vw::PixelMask<float>> & crop_ortho,
    vw::cartography::GeoReference & crop_ortho_georef) {

    // Find the bounding box of the dem and ortho portions seen in the camera,
    // in projected coordinates
    float mean_gsd = 0.0;    
    boost::shared_ptr<vw::camera::CameraModel> 
      camera_model(new vw::camera::PinholeModel(cam)); // expected by the API
    bool quick = true; // Assumes a big DEM fully containing the image    
    vw::BBox2 dem_box = vw::cartography::camera_bbox(dem, dem_georef, dem_georef,
      camera_model, image_size[0], image_size[1], mean_gsd, quick);
    vw::cartography::GeoTransform d2o(dem_georef, ortho_georef);
    vw::BBox2 ortho_box = d2o.point_to_point_bbox(dem_box);

    // Find the DEM pixel box and expand it in case there was some inaccuracies
    // in finding the box
    vw::BBox2i dem_pixel_box = dem_georef.point_to_pixel_bbox(dem_box);
    int expand = 50;
    dem_pixel_box.expand(expand);
    dem_pixel_box.crop(vw::bounding_box(dem));

    // Same for the ortho
    vw::BBox2i ortho_pixel_box = ortho_georef.point_to_pixel_bbox(ortho_box);
    ortho_pixel_box.expand(expand);
    ortho_pixel_box.crop(vw::bounding_box(ortho));

    // Crop
    crop_dem = vw::crop(dem, dem_pixel_box);
    crop_dem_georef = crop(dem_georef, dem_pixel_box);
    crop_ortho = vw::crop(ortho, ortho_pixel_box);
    crop_ortho_georef = crop(ortho_georef, ortho_pixel_box);
}

// Generate images by projecting rays from the sensor to the ground
void genImages(SatSimOptions const& opt,
    bool external_cameras,
    std::vector<std::string> const& cam_names,
    std::vector<vw::camera::PinholeModel> const& cams,
    vw::cartography::GeoReference const& dem_georef,
    vw::ImageViewRef<vw::PixelMask<float>> dem,
    vw::cartography::GeoReference const& ortho_georef,
    vw::ImageViewRef<vw::PixelMask<float>> ortho,
    float ortho_nodata_val) {

  vw::vw_out() << "Generating images.\n";

  // Generate image names from camera names by replacing the extension
  std::vector<std::string> image_names;
  image_names.resize(cam_names.size());
  for (int i = 0; i < int(cam_names.size()); i++) {
    if (external_cameras)
      image_names[i] = opt.out_prefix + "-" 
      + fs::path(cam_names[i]).filename().replace_extension(".tif").string();
    else
      image_names[i] = genPrefix(opt, i) + ".tif";
  }

  for (size_t i = 0; i < cams.size(); i++) {

    // Check if we do a range
    if (skipCamera(i, opt)) continue;

    // Bring crops in memory. It greatly helps with multi-threading speed.  
    vw::ImageView<vw::PixelMask<float>> crop_dem, crop_ortho;
    vw::cartography::GeoReference crop_dem_georef, crop_ortho_georef;
    setupCroppedDemAndOrtho(opt.image_size,
      cams[i], dem, dem_georef, ortho, ortho_georef, 
      // Outputs
      crop_dem, crop_dem_georef, crop_ortho, crop_ortho_georef);

    // Save the image using the block write function with multiple threads
    vw::vw_out() << "Writing: " << image_names[i] << std::endl;
    bool has_georef = false; // the produced image is raw, it has no georef
    bool has_nodata = true;
    block_write_gdal_image(image_names[i], 
      vw::apply_mask(SynImageView(opt, cams[i], 
      crop_dem_georef, crop_dem, crop_ortho_georef, crop_ortho, ortho_nodata_val), ortho_nodata_val),
      has_georef, crop_ortho_georef,  // the ortho georef will not be used
      has_nodata, ortho_nodata_val,   // borrow the nodata from ortho
      opt, vw::TerminalProgressCallback("", "\t--> "));
  }  

  return;
}

} // end namespace asp