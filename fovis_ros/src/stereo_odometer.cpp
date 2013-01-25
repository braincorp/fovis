#include <ros/ros.h>
#include <sensor_msgs/image_encodings.h>
#include <image_geometry/stereo_camera_model.h>
#include <cv_bridge/cv_bridge.h>
#include <nav_msgs/Odometry.h>
#include <geometry_msgs/PoseStamped.h>

#include <fovis/visual_odometry.hpp>
#include <fovis/stereo_depth.hpp>

#include <tf/transform_listener.h>
#include <tf/transform_broadcaster.h>

#include "stereo_processor.h"

namespace fovis_ros
{

class StereoOdometer : public StereoProcessor
{

private:

  boost::shared_ptr<fovis::VisualOdometry> visual_odometer_;
  fovis::VisualOdometryOptions visual_odometer_options_;
  boost::shared_ptr<fovis::StereoDepth> stereo_depth_;

  ros::Time last_time_;

  // tf related
  std::string sensor_frame_id_;
  std::string odom_frame_id_;
  std::string base_link_frame_id_;
  bool publish_tf_;
  tf::TransformListener tf_listener_;
  tf::TransformBroadcaster tf_broadcaster_;

  // publisher
  ros::Publisher odom_pub_;
  ros::Publisher pose_pub_;

public:

  StereoOdometer(const std::string& transport) : 
    StereoProcessor(transport), 
    visual_odometer_options_(fovis::VisualOdometry::getDefaultOptions())
  {
    // load parameters from node handle to visual_odometer_options_
    ros::NodeHandle local_nh("~");

    local_nh.param("odom_frame_id", odom_frame_id_, std::string("/odom"));
    local_nh.param("base_link_frame_id", base_link_frame_id_, std::string("/base_link"));
    local_nh.param("sensor_frame_id", sensor_frame_id_, std::string("/camera"));
    local_nh.param("publish_tf", publish_tf_, true);

    odom_pub_ = local_nh.advertise<nav_msgs::Odometry>("odometry", 1);
    pose_pub_ = local_nh.advertise<geometry_msgs::PoseStamped>("pose", 1);
  }

protected:

  void initOdometer(
      const sensor_msgs::CameraInfoConstPtr& l_info_msg,
      const sensor_msgs::CameraInfoConstPtr& r_info_msg)
  {
    // read calibration info from camera info message
    // to fill remaining parameters
    image_geometry::StereoCameraModel model;
    model.fromCameraInfo(*l_info_msg, *r_info_msg);
    
    // initialize left camera parameters
    fovis::CameraIntrinsicsParameters left_parameters;
    left_parameters.cx = model.left().cx();
    left_parameters.cy = model.left().cy();
    left_parameters.fx = model.left().fx();
    left_parameters.fy = model.left().fy();
    left_parameters.height = l_info_msg->height;
    left_parameters.width = l_info_msg->width;
    // initialize right camera parameters
    fovis::CameraIntrinsicsParameters right_parameters;
    right_parameters.cx = model.right().cx();
    right_parameters.cy = model.right().cy();
    right_parameters.fx = model.right().fx();
    right_parameters.fy = model.right().fy();
    right_parameters.height = r_info_msg->height;
    right_parameters.width = r_info_msg->width;

    fovis::StereoCalibrationParameters stereo_parameters;
    stereo_parameters.left_parameters = left_parameters;
    stereo_parameters.right_parameters = right_parameters;
    stereo_parameters.right_to_left_rotation[0] = 1.0;
    stereo_parameters.right_to_left_rotation[1] = 0.0;
    stereo_parameters.right_to_left_rotation[2] = 0.0;
    stereo_parameters.right_to_left_rotation[3] = 0.0;
    stereo_parameters.right_to_left_translation[0] = -model.baseline();
    stereo_parameters.right_to_left_translation[1] = 0.0;
    stereo_parameters.right_to_left_translation[2] = 0.0;

    fovis::StereoCalibration* stereo_calibration = new fovis::StereoCalibration(stereo_parameters);
    fovis::Rectification* rectification = new fovis::Rectification(left_parameters);

    stereo_depth_.reset(new fovis::StereoDepth(stereo_calibration, visual_odometer_options_));
    visual_odometer_.reset(new fovis::VisualOdometry(rectification, visual_odometer_options_));
    ROS_INFO_STREAM("Initialized fovis stereo odometry ");
  }

  void mountAndPublish(const Eigen::Isometry3d& pose, const Eigen::MatrixXd& motion_cov,
      const ros::Time& timestamp)
  {

    Eigen::Vector3d translation = pose.translation();
    Eigen::Quaterniond rotation(pose.rotation());

    nav_msgs::Odometry odom_msg;
    odom_msg.header.stamp = timestamp;
    odom_msg.header.frame_id = odom_frame_id_;
    odom_msg.child_frame_id = base_link_frame_id_;
    odom_msg.pose.pose.position.x = translation.x();
    odom_msg.pose.pose.position.y = translation.y();
    odom_msg.pose.pose.position.z = translation.z();
    odom_msg.pose.pose.orientation.x = rotation.x();
    odom_msg.pose.pose.orientation.y = rotation.y();
    odom_msg.pose.pose.orientation.z = rotation.z();
    odom_msg.pose.pose.orientation.w = rotation.w();

    if (!last_time_.isZero())
    {
      float dt = (timestamp - last_time_).toSec();
      if (dt > 0.0)
      {
        const Eigen::Isometry3d& last_motion = 
          visual_odometer_->getMotionEstimate();
        Eigen::Vector3d last_translation = last_motion.translation();
        Eigen::AngleAxisd aa(last_motion.rotation());
        odom_msg.twist.twist.linear.x = last_translation.x() / dt;
        odom_msg.twist.twist.linear.y = last_translation.y() / dt;
        odom_msg.twist.twist.linear.z = last_translation.z() / dt;
        odom_msg.twist.twist.angular.x = aa.axis().x() * aa.angle() / dt;
        odom_msg.twist.twist.angular.y = aa.axis().y() * aa.angle() / dt;
        odom_msg.twist.twist.angular.z = aa.axis().z() * aa.angle() / dt;
      }
    }

    for (int i=0;i<6;i++)
      for (int j=0;j<6;j++)
        odom_msg.twist.covariance[j*6+i] = motion_cov(i,j);
    // TODO integrate covariance for pose covariance
    
    odom_pub_.publish(odom_msg);
    geometry_msgs::PoseStamped pose_msg;
    pose_msg.pose = odom_msg.pose.pose;
    pose_msg.header = odom_msg.header;
    pose_msg.header.frame_id = odom_msg.child_frame_id;
    pose_pub_.publish(pose_msg);

    // if user wants to publish the tf
    if (publish_tf_)
    {
      // transform pose to base frame
      tf::StampedTransform base_to_sensor;
      std::string error_msg;
      if (tf_listener_.canTransform(base_link_frame_id_, sensor_frame_id_, ros::Time(0), &error_msg))
      {
        tf_listener_.lookupTransform(
            base_link_frame_id_,
            sensor_frame_id_,
            ros::Time(0), base_to_sensor);
      }
      else
      {
        ROS_WARN_THROTTLE(10.0, "The tf from '%s' to '%s' does not seem to be available, "
                                "will assume it as identity!", 
                                base_link_frame_id_.c_str(),
                                sensor_frame_id_.c_str());
        ROS_DEBUG("Transform error: %s", error_msg.c_str());
        base_to_sensor.setIdentity();
      }

      // compute the transform
      tf::Quaternion pose_quaternion = tf::Quaternion(rotation.x(), rotation.y(), rotation.z(), rotation.w());
      tf::Vector3 pose_origin = tf::Vector3(translation.x(), translation.y(), translation.z());
      tf::Transform pose_transform = tf::Transform(pose_quaternion, pose_origin);
      ROS_INFO_STREAM("Initialized fovis stereo odometry ");

      tf::Transform base_transform = base_to_sensor * pose_transform * base_to_sensor.inverse();

      // publish transform
      tf_broadcaster_.sendTransform(
          tf::StampedTransform(base_transform, timestamp,
          odom_frame_id_, base_link_frame_id_));
    }
    
    last_time_ = timestamp;
  }
 
  void imageCallback(
      const sensor_msgs::ImageConstPtr& l_image_msg,
      const sensor_msgs::ImageConstPtr& r_image_msg,
      const sensor_msgs::CameraInfoConstPtr& l_info_msg,
      const sensor_msgs::CameraInfoConstPtr& r_info_msg)
  {
    bool first_run = false;
    // create odometer if not exists
    if (!visual_odometer_)
    {
      initOdometer(l_info_msg, r_info_msg);
      first_run = true;
    }

    // convert images if necessary
    uint8_t *l_image_data, *r_image_data;
    int l_step, r_step;
    cv_bridge::CvImageConstPtr l_cv_ptr, r_cv_ptr;
    l_cv_ptr = cv_bridge::toCvShare(l_image_msg, sensor_msgs::image_encodings::MONO8);
    l_image_data = l_cv_ptr->image.data;
    l_step = l_cv_ptr->image.step[0];
    r_cv_ptr = cv_bridge::toCvShare(r_image_msg, sensor_msgs::image_encodings::MONO8);
    r_image_data = r_cv_ptr->image.data;
    r_step = r_cv_ptr->image.step[0];

    ROS_ASSERT(l_step == r_step);
    ROS_ASSERT(l_step == static_cast<int>(l_image_msg->width));
    ROS_ASSERT(l_image_msg->width == r_image_msg->width);
    ROS_ASSERT(l_image_msg->height == r_image_msg->height);

    // pass images to odometer
    stereo_depth_->setRightImage(r_image_data);
    visual_odometer_->processFrame(l_image_data, stereo_depth_.get());

    fovis::MotionEstimateStatusCode status = 
      visual_odometer_->getMotionEstimateStatus();

    if (status == fovis::SUCCESS)
    {
      // get pose and motion
      const Eigen::Isometry3d& pose = visual_odometer_->getPose();
      const Eigen::MatrixXd& motion_cov = visual_odometer_->getMotionEstimateCov();

      // Publish pose, odometry and tf
      mountAndPublish(pose, motion_cov, l_image_msg->header.stamp);
    }
    else
    {
      ROS_ERROR_STREAM("fovis stereo odometry failed: " << 
          fovis::MotionEstimateStatusCodeStrings[status]);
    }
  }
};

} // end of namespace


int main(int argc, char **argv)
{
  ros::init(argc, argv, "stereo_odometer");
  if (ros::names::remap("stereo") == "stereo") {
    ROS_WARN("'stereo' has not been remapped! Example command-line usage:\n"
             "\t$ rosrun fovis_ros stereo_odometer stereo:=narrow_stereo image:=image_rect");
  }
  if (ros::names::remap("image").find("rect") == std::string::npos) {
    ROS_WARN("stereo_odometer needs rectified input images. The used image "
             "topic is '%s'. Are you sure the images are rectified?",
             ros::names::remap("image").c_str());
  }

  std::string transport = argc > 1 ? argv[1] : "raw";
  fovis_ros::StereoOdometer odometer(transport);
  
  ros::spin();
  return 0;
}

