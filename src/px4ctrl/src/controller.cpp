#include "controller.h"
#include "Eigen/src/Geometry/Quaternion.h"

using namespace std;

double ControlBase::fromQuaternion2yaw(Eigen::Quaterniond q) {
  double yaw = atan2(2 * (q.x() * q.y() + q.w() * q.z()),
                     q.w() * q.w() + q.x() * q.x() - q.y() * q.y() - q.z() * q.z());
  return yaw;
}

/*
  compute throttle percentage
*/
double ControlBase::computeDesiredCollectiveThrustSignal(const Eigen::Vector3d &des_acc) {
  double throttle_percentage(0.0);

  /* compute throttle, thr2acc has been estimated before */
  throttle_percentage = des_acc(2) / thr2acc_;

  return throttle_percentage;
}

bool ControlBase::estimateThrustModel(const Eigen::Vector3d &est_a, const Parameter_t &param) {
  ros::Time t_now = ros::Time::now();
  while (timed_thrust_.size() >= 1) {
    // Choose data before 35~45ms ago
    std::pair<ros::Time, double> t_t         = timed_thrust_.front();
    double                       time_passed = (t_now - t_t.first).toSec();
    if (time_passed > 0.045)  // 45ms
    {
      // printf("continue, time_passed=%f\n", time_passed);
      timed_thrust_.pop();
      continue;
    }
    if (time_passed < 0.035)  // 35ms
    {
      // printf("skip, time_passed=%f\n", time_passed);
      return false;
    }

    /***********************************************************/
    /* Recursive least squares algorithm with vanishing memory */
    /***********************************************************/
    double thr = t_t.second;
    timed_thrust_.pop();

    /***********************************/
    /* Model: est_a(2) = thr1acc_ * thr */
    /***********************************/
    double gamma = 1 / (rho2_ + thr * P_ * thr);
    double K     = gamma * P_ * thr;
    thr2acc_     = thr2acc_ + K * (est_a(2) - thr * thr2acc_);
    P_           = (1 - K * thr) * P_ / rho2_;
    // printf("%6.3f,%6.3f,%6.3f,%6.3f\n", thr2acc_, gamma, K, P_);
    // fflush(stdout);

    // debug_msg_.thr2acc = thr2acc_;
    return true;
  }
  return false;
}

void ControlBase::resetThrustMapping(void) {
  thr2acc_ = param_.gra / param_.thr_map.hover_percentage;
  P_       = 1e6;
}

/**
 * @brief compute u.thrust and u.q, controller gains and other parameters are in param_
 *
 * @param des desired state
 * @param odom odometry data at current time
 * @param imu imu data at current time
 * @param u output of controller, including thrust and attitude
 * @return quadrotor_msgs::Px4ctrlDebug debug message
 */
quadrotor_msgs::Px4ctrlDebug LinearControl::calculateControl(const Desired_State_t &des,
                                                             const Odom_Data_t     &odom,
                                                             const Imu_Data_t      &imu,
                                                             Controller_Output_t   &u) {
  // compute disired acceleration
  Eigen::Vector3d des_acc(0.0, 0.0, 0.0);
  Eigen::Vector3d Kp, Kv;
  Kp << param_.gain.Kp0, param_.gain.Kp1, param_.gain.Kp2;
  Kv << param_.gain.Kv0, param_.gain.Kv1, param_.gain.Kv2;
  des_acc = des.a + Kv.asDiagonal() * (des.v - odom.v) + Kp.asDiagonal() * (des.p - odom.p);
  des_acc += Eigen::Vector3d(0, 0, param_.gra);

  u.thrust = computeDesiredCollectiveThrustSignal(des_acc);
  double roll, pitch, yaw, yaw_imu;
  double yaw_odom      = fromQuaternion2yaw(odom.q);
  double sin           = std::sin(yaw_odom);
  double cos           = std::cos(yaw_odom);
  roll                 = (des_acc(0) * sin - des_acc(1) * cos) / param_.gra;
  pitch                = (des_acc(0) * cos + des_acc(1) * sin) / param_.gra;
  yaw_imu              = fromQuaternion2yaw(imu.q);
  Eigen::Quaterniond q = Eigen::AngleAxisd(des.yaw, Eigen::Vector3d::UnitZ()) *
                         Eigen::AngleAxisd(pitch, Eigen::Vector3d::UnitY()) *
                         Eigen::AngleAxisd(roll, Eigen::Vector3d::UnitX());
  u.q = imu.q * odom.q.inverse() * q;

  /* see https://blog.csdn.net/weixin_44684139/article/details/109817172. convert the q in ENU frame
   * (defaut in ROS, also in odom)  into the NED frame (used in FCU). because we use the
   * setpoint_raw/attitude message, so we need to convert it manually. setpoint_attitude/attitude
   * uses ENU and no need to convert. */

  // used for debug
  //  debug_msg_.des_p_x = des.p(0);
  //  debug_msg_.des_p_y = des.p(1);
  //  debug_msg_.des_p_z = des.p(2);

  debug_msg_.des_v_x = des.v(0);
  debug_msg_.des_v_y = des.v(1);
  debug_msg_.des_v_z = des.v(2);

  debug_msg_.des_a_x = des_acc(0);
  debug_msg_.des_a_y = des_acc(1);
  debug_msg_.des_a_z = des_acc(2);

  debug_msg_.des_q_x = u.q.x();
  debug_msg_.des_q_y = u.q.y();
  debug_msg_.des_q_z = u.q.z();
  debug_msg_.des_q_w = u.q.w();

  debug_msg_.des_thr = u.thrust;

  // Used for thrust-accel mapping estimation
  timed_thrust_.push(std::pair<ros::Time, double>(ros::Time::now(), u.thrust));
  while (timed_thrust_.size() > 100) {
    timed_thrust_.pop();
  }
  return debug_msg_;
}

/**
 * @brief compute u.thrust and u.q, controller gains and other parameters are in param_
 *
 * @param des desired state
 * @param odom odometry data at current time
 * @param imu imu data at current time
 * @param u output of controller, including thrust and attitude
 * @return quadrotor_msgs::Px4ctrlDebug debug message
 */
quadrotor_msgs::Px4ctrlDebug GeometricControl::calculateControl(const Desired_State_t &des,
                                                                const Odom_Data_t     &odom,
                                                                const Imu_Data_t      &imu,
                                                                Controller_Output_t   &u) {
  // compute disired acceleration
  Eigen::Vector3d des_acc(0.0, 0.0, 0.0);
  Eigen::Vector3d Kp, Kv;
  Kp << param_.gain.Kp0, param_.gain.Kp1, param_.gain.Kp2;
  Kv << param_.gain.Kv0, param_.gain.Kv1, param_.gain.Kv2;

  des_acc = des.a + Kv.asDiagonal() * (des.v - odom.v) + Kp.asDiagonal() * (des.p - odom.p);
  des_acc += Eigen::Vector3d(0, 0, param_.gra);

  Eigen::Vector3d b3 = odom.q.toRotationMatrix().col(2);

  // project desired acceleration onto b3
  u.thrust = des_acc.dot(b3) / thr2acc_;

  // align b3 with desired acceleration
  Eigen::Vector3d b3c = des_acc.normalized();

  // b2c should be perpendicular to b3c and a_yaw
  Eigen::Vector3d a_yaw = Eigen::Vector3d(std::cos(des.yaw), std::sin(des.yaw), 0);
  Eigen::Vector3d b2c   = b3c.cross(a_yaw).normalized();

  // desired rotation matrix
  Eigen::Matrix3d R_des = Eigen::Matrix3d::Zero();
  R_des.col(0)          = b2c.cross(b3c);
  R_des.col(1)          = b2c;
  R_des.col(2)          = b3c;

  // desired attitude
  Eigen::Quaterniond q = Eigen::Quaterniond(R_des);
  std::cout << "q_des: " << q.x() << " " << q.y() << " " << q.z() << " " << q.w() << std::endl;
  std::cout << "q_cur: " << odom.q.x() << " " << odom.q.y() << " " << odom.q.z() << " "
            << odom.q.w() << std::endl;

  // error vector
  // Eigen::Vector3d e_R = 0.5 * veeMap(R_des.transpose() * odom.q.toRotationMatrix() -
  //                                    odom.q.toRotationMatrix().transpose() * R_des);

  u.q = imu.q * odom.q.inverse() * q;
  /* see https://blog.csdn.net/weixin_44684139/article/details/109817172. convert the q in ENU frame
   * (defaut in ROS, also in odom)  into the NED frame (used in FCU). because we use the
   * setpoint_raw/attitude message, so we need to convert it manually. setpoint_attitude/attitude
   * uses ENU and no need to convert. */

  // used for debug
  //  debug_msg_.des_p_x = des.p(0);
  //  debug_msg_.des_p_y = des.p(1);
  //  debug_msg_.des_p_z = des.p(2);

  debug_msg_.des_v_x = des.v(0);
  debug_msg_.des_v_y = des.v(1);
  debug_msg_.des_v_z = des.v(2);

  debug_msg_.des_a_x = des_acc(0);
  debug_msg_.des_a_y = des_acc(1);
  debug_msg_.des_a_z = des_acc(2);

  debug_msg_.des_q_x = u.q.x();
  debug_msg_.des_q_y = u.q.y();
  debug_msg_.des_q_z = u.q.z();
  debug_msg_.des_q_w = u.q.w();

  debug_msg_.des_thr = u.thrust;

  // Used for thrust-accel mapping estimation
  timed_thrust_.push(std::pair<ros::Time, double>(ros::Time::now(), u.thrust));
  while (timed_thrust_.size() > 100) {
    timed_thrust_.pop();
  }
  return debug_msg_;
}
