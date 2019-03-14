#include <chrono>

#include "gazebo/gazebo.hh"
#include "gazebo/physics/physics.hh"

#include "gazebo_ros/node.hpp"
#include "geometry_msgs/msg/twist.hpp"
#include "tello_msgs/msg/flight_data.hpp"
#include "tello_msgs/msg/tello_response.hpp"
#include "tello_msgs/srv/tello_action.hpp"

#include "pid.hpp"

using namespace std::chrono_literals;

namespace tello_gazebo {

const bool DEBUG = false;

const double MAX_XY_V = 8.0;
const double MAX_Z_V = 4.0;
const double MAX_ANG_V = M_PI;

const double MAX_XY_A = 8.0;
const double MAX_Z_A = 4.0;
const double MAX_ANG_A = M_PI;

inline double clamp(const double v, const double max)
{
  return v > max ? max : (v < -max ? -max : v);
}

class TelloPlugin : public gazebo::ModelPlugin
{
  //gazebo::physics::Model model_;
  gazebo::physics::LinkPtr base_link_;

  // Force will be applied to the center_of_mass_ (body frame)
  // TODO is it possible to get this from the link?
  ignition::math::Vector3d center_of_mass_ {0, 0, 0};

  // Connection to Gazebo message bus
  gazebo::event::ConnectionPtr update_connection_;

  // GazeboROS node
  gazebo_ros::Node::SharedPtr node_;

  // ROS publishers
  rclcpp::Publisher<tello_msgs::msg::FlightData>::SharedPtr flight_data_pub_;
  rclcpp::Publisher<tello_msgs::msg::TelloResponse >::SharedPtr tello_response_pub_;

  // ROS services
  rclcpp::Service<tello_msgs::srv::TelloAction>::SharedPtr command_srv_;

  // ROS subscriptions
  rclcpp::Subscription<geometry_msgs::msg::Twist>::SharedPtr cmd_vel_sub_;

  // ROS timer
  rclcpp::TimerBase::SharedPtr timer_;

  // Sim time of last update
  gazebo::common::Time sim_time_;

  // cmd_vel messages control x, y, z and yaw velocity
  pid::Controller x_controller_ {false, 2, 0, 0};
  pid::Controller y_controller_ {false, 2, 0, 0};
  pid::Controller z_controller_ {false, 2, 0, 0};
  pid::Controller yaw_controller_ {false, 2, 0, 0};

public:

  TelloPlugin()
  {
  }

  ~TelloPlugin()
  {
  }

  // Called once when the plugin is loaded.
  void Load(gazebo::physics::ModelPtr model, sdf::ElementPtr sdf)
  {
    GZ_ASSERT(model != nullptr, "Model is null");
    GZ_ASSERT(sdf != nullptr, "SDF is null");

    std::string ns;
    std::string link_name {"base_link"};

    // In theory we can move much of this config into the <ros> tag, but this appears unfinished in Crystal
    if (sdf->HasElement("ns")) {
      ns = sdf->GetElement("ns")->Get<std::string>();
    }
    if (sdf->HasElement("link_name")) {
      link_name = sdf->GetElement("link_name")->Get<std::string>();
    }
    if (sdf->HasElement("center_of_mass")) {
      center_of_mass_ = sdf->GetElement("center_of_mass")->Get<ignition::math::Vector3d>();
    }

    std::cout << std::fixed;
    std::setprecision(2);

    std::cout << std::endl;
    std::cout << "TELLO PLUGIN" << std::endl;
    std::cout << "-----------------------------------------" << std::endl;
    std::cout << "ns: " << ns << std::endl;
    std::cout << "link_name: " << link_name << std::endl;
    std::cout << "center_of_mass: " << center_of_mass_ << std::endl;
    std::cout << "-----------------------------------------" << std::endl;
    std::cout << std::endl;

    base_link_ = model->GetLink(link_name);
    GZ_ASSERT(base_link_ != nullptr, "Missing link");

    // ROS node
    node_ = gazebo_ros::Node::Get(sdf);

    if (!ns.empty()) {
      ns.append("/");
    }

    // ROS publishers
    flight_data_pub_ = node_->create_publisher<tello_msgs::msg::FlightData>(ns + "flight_data", 1);
    tello_response_pub_ = node_->create_publisher<tello_msgs::msg::TelloResponse>(ns + "tello_response", 1);

    // ROS service
    command_srv_ = node_->create_service<tello_msgs::srv::TelloAction>(ns + "tello_action",
      std::bind(&TelloPlugin::command_callback, this,
      std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

    // ROS subscription
    cmd_vel_sub_ = node_->create_subscription<geometry_msgs::msg::Twist>(ns + "cmd_vel",
      std::bind(&TelloPlugin::cmd_vel_callback, this, std::placeholders::_1));

    // 10Hz ROS timer
    timer_ = node_->create_wall_timer(100ms, std::bind(&TelloPlugin::spin_10Hz, this));

    // Listen for the Gazebo update event. This event is broadcast every simulation iteration.
    update_connection_ = gazebo::event::Events::ConnectWorldUpdateBegin(boost::bind(&TelloPlugin::OnUpdate, this, _1));
  }

  // Called by the world update start event, up to 1000 times per second.
  void OnUpdate(const gazebo::common::UpdateInfo& info)
  {
    static int count = 0;
    bool debug = false;
    if (++count > 100) {
      debug = DEBUG;
      count = 0;
    }

    // dt
    double dt = (info.simTime - sim_time_).Double();
    sim_time_ = info.simTime;

    // Get current velocity
    ignition::math::Vector3d linear_velocity = base_link_->RelativeLinearVel();
    ignition::math::Vector3d angular_velocity = base_link_->RelativeAngularVel();

    if (debug) {
      std::cout << "linear v: " << linear_velocity.X() << ", " << linear_velocity.Y() << ", " << linear_velocity.Z() << std::endl;
      std::cout << "angular v: " << angular_velocity.X() << ", " << angular_velocity.Y() << ", " << angular_velocity.Z() << std::endl;
    }

    // Calc desired acceleration (ubar)
    ignition::math::Vector3d lin_ubar, ang_ubar;
    lin_ubar.X(x_controller_.calc(linear_velocity.X(), dt, 0));
    lin_ubar.Y(y_controller_.calc(linear_velocity.Y(), dt, 0));
    lin_ubar.Z(z_controller_.calc(linear_velocity.Z(), dt, 0));
    ang_ubar.Z(yaw_controller_.calc(angular_velocity.Z(), dt, 0));

    if (debug) {
      std::cout << "lin_ubar: " << lin_ubar.X() << ", " << lin_ubar.Y() << ", " << lin_ubar.Z() << std::endl;
      std::cout << "ang_ubar: " << ang_ubar.X() << ", " << ang_ubar.Y() << ", " << ang_ubar.Z() << std::endl;
    }

    // Clamp acceleration
    lin_ubar.X() = clamp(lin_ubar.X(), MAX_XY_A);
    lin_ubar.Y() = clamp(lin_ubar.Y(), MAX_XY_A);
    lin_ubar.Z() = clamp(lin_ubar.Z(), MAX_Z_A);
    ang_ubar.Z() = clamp(ang_ubar.Z(), MAX_ANG_A);

    if (debug) {
      std::cout << "lin_ubar clamped: " << lin_ubar.X() << ", " << lin_ubar.Y() << ", " << lin_ubar.Z() << std::endl;
      std::cout << "ang_ubar clamped: " << ang_ubar.X() << ", " << ang_ubar.Y() << ", " << ang_ubar.Z() << std::endl;
    }

    // Calc force and torque
    ignition::math::Vector3d force = lin_ubar * base_link_->GetInertial()->Mass();
    ignition::math::Vector3d torque = ang_ubar * base_link_->GetInertial()->MOI();

    if (debug) {
      std::cout << "force: " << force.X() << ", " << force.Y() << ", " << force.Z() << std::endl;
      std::cout << "torque: " << torque.X() << ", " << torque.Y() << ", " << torque.Z() << std::endl;
      std::cout << std::endl;
    }

    // Set roll and pitch to 0
    ignition::math::Pose3d pose = base_link_->WorldPose();
    pose.Rot().X(0);
    pose.Rot().Y(0);
    base_link_->SetWorldPose(pose);

    // Apply force and torque
    base_link_->AddLinkForce(force, center_of_mass_);
    base_link_->AddRelativeTorque(torque); // ODE adds torque at the center of mass
  }

  void command_callback(
    const std::shared_ptr<rmw_request_id_t> request_header,
    const std::shared_ptr<tello_msgs::srv::TelloAction::Request> request,
    std::shared_ptr<tello_msgs::srv::TelloAction::Response> response)
  {
    // TODO initiate command
  }

  void cmd_vel_callback(const geometry_msgs::msg::Twist::SharedPtr msg)
  {
    // std::cout << std::endl << "target v: " << msg->linear.x << ", " << msg->linear.y << ", " << msg->linear.z << std::endl << std::endl;

    // TODO cmd_vel should specify velocity, not joystick position

    x_controller_.set_target(msg->linear.x * MAX_XY_V);
    y_controller_.set_target(msg->linear.y * MAX_XY_V);
    z_controller_.set_target(msg->linear.z * MAX_Z_V);
    yaw_controller_.set_target(msg->angular.z * MAX_ANG_V);
  }

  void spin_10Hz()
  {
    // Minimal flight data
    tello_msgs::msg::FlightData flight_data;
    flight_data.header.stamp = node_->now();
    flight_data.sdk = flight_data.SDK_1_3;
    flight_data.bat = 80;
    flight_data_pub_->publish(flight_data);

    // TODO publish tello response
  }
};

GZ_REGISTER_MODEL_PLUGIN(TelloPlugin)

} // namespace tello_gazebo