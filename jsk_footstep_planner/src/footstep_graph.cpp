// -*- mode: c++ -*-
/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2015, JSK Lab
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/o2r other materials provided
 *     with the distribution.
 *   * Neither the name of the JSK Lab nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

#include "jsk_footstep_planner/footstep_graph.h"
#include <sstream>

namespace jsk_footstep_planner
{
  void FootstepGraph::setBasicSuccessors(
    std::vector<Eigen::Affine3f> left_to_right_successors)
  {
    successors_from_left_to_right_ = left_to_right_successors;
    for (size_t i = 0; i < successors_from_left_to_right_.size(); i++) {
      Eigen::Affine3f transform = successors_from_left_to_right_[i];
      float roll, pitch, yaw;
      pcl::getEulerAngles(transform, roll, pitch, yaw);
      Eigen::Vector3f translation = transform.translation();
      Eigen::Affine3f flipped_transform
        = Eigen::Translation3f(translation[0], -translation[1], translation[2])
        * Eigen::Quaternionf(Eigen::AngleAxisf(-yaw, Eigen::Vector3f::UnitZ()));
      successors_from_right_to_left_.push_back(flipped_transform);
    }

    // max_successor_distance_
    for (size_t i = 0; i < successors_from_left_to_right_.size(); i++) {
      Eigen::Affine3f transform = successors_from_left_to_right_[i];
      //double dist = transform.translation().norm();
      double dist = transform.translation()[0]; // Only consider x
      if (dist > max_successor_distance_) {
        max_successor_distance_ = dist;
      }
      double rot = Eigen::AngleAxisf(transform.rotation()).angle();
      if (rot > max_successor_rotation_) {
        max_successor_rotation_ = rot;
      }
    }
  }

  bool FootstepGraph::isGoal(StatePtr state)
  {
    FootstepState::Ptr goal = getGoal(state->getLeg());
    if (publish_progress_) {
      jsk_footstep_msgs::FootstepArray msg;
      msg.header.frame_id = "odom"; // TODO fixed frame_id
      msg.header.stamp = ros::Time::now();
      msg.footsteps.push_back(*state->toROSMsg());
      pub_progress_.publish(msg);
    }
    Eigen::Affine3f pose = state->getPose();
    Eigen::Affine3f goal_pose = goal->getPose();
    Eigen::Affine3f transformation = pose.inverse() * goal_pose;

    if ((parameters_.goal_pos_thr > transformation.translation().norm()) &&
        (parameters_.goal_rot_thr > std::abs(Eigen::AngleAxisf(transformation.rotation()).angle()))) {
      // check collision
      if (state->getLeg() == jsk_footstep_msgs::Footstep::LEFT) {
        if (right_goal_state_->crossCheck(state)) {
          return true;
        }
      } else if (state->getLeg() == jsk_footstep_msgs::Footstep::RIGHT) {
        if (left_goal_state_->crossCheck(state)) {
          return true;
        }
      }
    }
    return false;
  }

  Eigen::Affine3f FootstepGraph::getRobotCoords(StatePtr current_state, StatePtr previous_state) const
  {
    Eigen::Affine3f mid = current_state->midcoords(*previous_state);
    return mid * collision_bbox_offset_;
  }

  pcl::PointIndices::Ptr FootstepGraph::getPointIndicesCollidingSphere(const Eigen::Affine3f& c)
  {
    pcl::PointXYZ center;
    center.getVector3fMap() = Eigen::Vector3f(c.translation());
    const double r = collision_bbox_size_.norm() / 2 + parameters_.obstacle_resolution;
    pcl::PointIndices::Ptr near_indices(new pcl::PointIndices);
    std::vector<float> distances;
    obstacle_tree_model_->radiusSearch(center, r, near_indices->indices, distances);
    return near_indices;
  }

  bool FootstepGraph::isCollidingBox(const Eigen::Affine3f& c, pcl::PointIndices::Ptr candidates) const
  {
    const pcl::PointCloud<pcl::PointXYZ>::ConstPtr input_cloud = obstacle_tree_model_->getInputCloud();
    Eigen::Affine3f inv_c = c.inverse();
    for (size_t i = 0; i < candidates->indices.size(); i++) {
      int index = candidates->indices[i];
      const pcl::PointXYZ candidate_point = input_cloud->points[index];
      // convert candidate_point into `c' local representation.
      const Eigen::Vector3f local_p = inv_c * candidate_point.getVector3fMap();
      if (std::abs(local_p[0]) < collision_bbox_size_[0] / 2 &&
          std::abs(local_p[1]) < collision_bbox_size_[1] / 2 &&
          std::abs(local_p[2]) < collision_bbox_size_[2] / 2) {
        return true;
      }
    }
    return false;
  }

  // return true if colliding with obstacle
  bool FootstepGraph::isColliding(StatePtr current_state, StatePtr previous_state)
  {
    // if not use obstacle model, always return false
    // if use obstacle model and obstacle_model_ point cloud size is zero, always return false
    // => to be collision-free always.
    if (!use_obstacle_model_ || (obstacle_model_->size() == 0) ) {
      return false;
    }
    // compute robot coorde
    Eigen::Affine3f robot_coords = getRobotCoords(current_state, previous_state);
    pcl::PointIndices::Ptr sphere_candidate = getPointIndicesCollidingSphere(robot_coords);
    if (sphere_candidate->indices.size() == 0) {
      return false;
    }
    return isCollidingBox(robot_coords, sphere_candidate);
  }

  bool FootstepGraph::finalizeSteps(const StatePtr &last_1_Step, const StatePtr &lastStep,
                                    std::vector<StatePtr> &finalizeSteps) {
    // simple finalize (no check)
    if (lastStep->getLeg() == jsk_footstep_msgs::Footstep::LEFT) {
      finalizeSteps.push_back(right_goal_state_);
      finalizeSteps.push_back(left_goal_state_);
    } else if (lastStep->getLeg() == jsk_footstep_msgs::Footstep::RIGHT) {
      finalizeSteps.push_back(left_goal_state_);
      finalizeSteps.push_back(right_goal_state_);
    }

    return true;
  }

  std::string FootstepGraph::infoString() const
  {
    std::stringstream ss;
    ss << "footstep_graph" << std::endl;
    ss << "  goal_pos_thr: " << parameters_.goal_pos_thr << std::endl;
    ss << "  goal_rot_thr: " << parameters_.goal_rot_thr << std::endl;
    ss << "  use_pointcloud_model: " << use_pointcloud_model_ << std::endl;
    ss << "  lazy_projection: " << lazy_projection_ << std::endl;
    ss << "  local_movement: " << local_movement_ << std::endl;
    ss << "  transition_limit: " << transition_limit_ << std::endl;
    if (global_transition_limit_) {
      ss << "  global_transition_limit: " << global_transition_limit_ << std::endl;
    }
    else {
      ss << "  global_transition_limit: None" << std::endl;
    }
    ss << "  local_move_x: " << parameters_.local_move_x << std::endl;
    ss << "  local_move_y: " << parameters_.local_move_y << std::endl;
    ss << "  local_move_theta: " << parameters_.local_move_theta << std::endl;
    ss << "  local_move_x_num: " << parameters_.local_move_x_num << std::endl;
    ss << "  local_move_y_num: " << parameters_.local_move_y_num << std::endl;
    ss << "  local_move_theta_num: " << parameters_.local_move_theta_num << std::endl;
    ss << "  resolution: ["
       << resolution_[0] << ", "
       << resolution_[1] << ", "
       << resolution_[2] << "]"
       << std::endl;
    ss << "  plane_estimation_use_normal: "             << parameters_.plane_estimation_use_normal             << std::endl;
    ss << "  plane_estimation_normal_distance_weight: " << parameters_.plane_estimation_normal_distance_weight << std::endl;
    ss << "  plane_estimation_normal_opening_angle: "   << parameters_.plane_estimation_normal_opening_angle   << std::endl;
    ss << "  plane_estimation_min_ratio_of_inliers: "   << parameters_.plane_estimation_min_ratio_of_inliers   << std::endl;
    ss << "  plane_estimation_max_iterations: " << parameters_.plane_estimation_max_iterations << std::endl;
    ss << "  plane_estimation_min_inliers: " << parameters_.plane_estimation_min_inliers << std::endl;
    ss << "  plane_estimation_outlier_threshold: " << parameters_.plane_estimation_outlier_threshold << std::endl;

    ss << "  support_check_x_sampling: " << parameters_.support_check_x_sampling << std::endl;
    ss << "  support_check_y_sampling: " << parameters_.support_check_y_sampling << std::endl;
    ss << "  support_check_vertex_neighbor_threshold: " << parameters_.support_check_vertex_neighbor_threshold << std::endl;
    ss << "  support_padding_x: " << parameters_.support_padding_x << std::endl;
    ss << "  support_padding_y: " << parameters_.support_padding_y << std::endl;
    ss << "  skip_cropping: " << parameters_.skip_cropping << std::endl;

    return ss.str();
  }

  bool
  FootstepGraph::isSuccessable(StatePtr current_state, StatePtr previous_state)
  {
    if (global_transition_limit_) {
      if (!global_transition_limit_->check(zero_state_, current_state)) {
        return false;
      }
    }
    if (transition_limit_) {
      if (!transition_limit_->check(previous_state, current_state)) {
        return false;
      }
    }
    if (use_obstacle_model_) {
      return !isColliding(current_state, previous_state);
    }
    return true;
  }

  std::vector<FootstepState::Ptr>
  FootstepGraph::localMoveFootstepState(FootstepState::Ptr in)
  {
    std::vector<FootstepState::Ptr> moved_states;
    moved_states.reserve((2*parameters_.local_move_x_num + 1)*(2*parameters_.local_move_y_num + 1)*(2*parameters_.local_move_theta_num +1) - 1);
    int x_num     = parameters_.local_move_x_num;
    int y_num     = parameters_.local_move_y_num;
    int theta_num = parameters_.local_move_theta_num;
    if(x_num == 0)     x_num = 1;
    if(y_num == 0)     y_num = 1;
    if(theta_num == 0) theta_num = 1;

    double move_x = parameters_.local_move_x;
    double move_y = parameters_.local_move_y;
    double move_t = parameters_.local_move_theta;
    double offset_x = parameters_.local_move_x_offset;
    double offset_y = (in->getLeg() == jsk_footstep_msgs::Footstep::LEFT) ?
      parameters_.local_move_y_offset : - parameters_.local_move_y_offset;
    double offset_t = parameters_.local_move_theta_offset;

    bool have_offset = ((offset_x != 0.0) || (offset_y != 0.0) || (offset_t != 0.0));
    for (int xi = - parameters_.local_move_x_num; xi <= parameters_.local_move_x_num; xi++) {
      for (int yi = - parameters_.local_move_y_num; yi <= parameters_.local_move_y_num; yi++) {
        for (int thetai = - parameters_.local_move_theta_num; thetai <= parameters_.local_move_theta_num; thetai++) {
          if ( have_offset || (xi != 0) || (yi != 0) || (thetai != 0) ) {
            Eigen::Affine3f trans(Eigen::Translation3f((move_x / x_num * xi) + offset_x,
                                                       (move_y / y_num * yi) + offset_y,
                                                       0)
                                  * Eigen::AngleAxisf((move_t / theta_num * thetai) + offset_t,
                                                      Eigen::Vector3f::UnitZ()));
            moved_states.push_back(
                                   FootstepState::Ptr(new FootstepState(in->getLeg(),
                                                                        in->getPose() * trans,
                                                                        in->getDimensions(),
                                                                        in->getResolution())));
          }
        }
      }
    }
    return moved_states;
  }

  bool FootstepGraph::successors_original(StatePtr target_state, std::vector<FootstepGraph::StatePtr> &ret)
  {
    std::vector<Eigen::Affine3f> transformations;
    int next_leg;
    if (target_state->getLeg() == jsk_footstep_msgs::Footstep::LEFT) {
      transformations = successors_from_left_to_right_;
      next_leg = jsk_footstep_msgs::Footstep::RIGHT;
    }
    else if (target_state->getLeg() == jsk_footstep_msgs::Footstep::RIGHT) {
      transformations = successors_from_right_to_left_;
      next_leg = jsk_footstep_msgs::Footstep::LEFT;
    }
    else {
      // TODO: error
    }

    //std::vector<FootstepGraph::StatePtr> ret;
    Eigen::Affine3f base_pose = target_state->getPose();
    for (size_t i = 0; i < transformations.size(); i++) {
      Eigen::Affine3f transform = transformations[i];
      FootstepGraph::StatePtr next(new FootstepState(next_leg,
                                                     base_pose * transform,
                                                     target_state->getDimensions(),
                                                     resolution_));
      if (use_pointcloud_model_ && !lazy_projection_ && !force_ignore_projection_) {
        // Update footstep position by projection
        unsigned int error_state;
        FootstepGraph::StatePtr tmpnext = projectFootstep(next, error_state);
        if (!tmpnext && localMovement() && error_state == projection_state::close_to_success) {
          std::vector<StatePtr> locally_moved_nodes = localMoveFootstepState(next);
          for (size_t j = 0; j < locally_moved_nodes.size(); j++) {
            if (isSuccessable(locally_moved_nodes[j], target_state)) {
              FootstepGraph::StatePtr tmp = projectFootstep(locally_moved_nodes[j], error_state);
              if(!!tmp) {
                ret.push_back(tmp);
              }
            }
          }
        }
        next = tmpnext;
      }
      if (!!next) {
        if (isSuccessable(next, target_state)) {
          ret.push_back(next);
        }
      }
    }
    return true;
  }

  FootstepState::Ptr FootstepGraph::projectFootstep(FootstepState::Ptr in)
  {
    unsigned int error_state;
    return projectFootstep(in, error_state);
  }

  FootstepState::Ptr FootstepGraph::projectFootstep(FootstepState::Ptr in,
                                                    unsigned int& error_state)
  {
    if(!use_pointcloud_model_) {
      return in;
    }
    ros::WallTime start_time = ros::WallTime::now();
    FootstepState::Ptr projected_footstep = in->projectToCloud(
      *tree_model_,
      pointcloud_model_,
      grid_search_,
      *tree_model_2d_,
      pointcloud_model_2d_,
      Eigen::Vector3f(0, 0, 1),
      error_state, parameters_);
    ros::WallTime end_time = ros::WallTime::now();
    perception_duration_ = perception_duration_ + (end_time  - start_time);
    return projected_footstep;
  }

  bool FootstepGraph::projectGoal()
  {
    unsigned int error_state;
    FootstepState::Ptr left_projected = projectFootstep(left_goal_state_);
    FootstepState::Ptr right_projected = projectFootstep(right_goal_state_);
    if (left_projected && right_projected) {
      if (global_transition_limit_) {
        if (!global_transition_limit_->check(zero_state_, left_projected) ||
            !global_transition_limit_->check(zero_state_, right_projected)) {
          return false;
        }
      }

      left_goal_state_ = left_projected;
      right_goal_state_ = right_projected;
      return true;
    }
    else {
      return false;
    }
  }

  bool FootstepGraph::projectStart()
  {
    unsigned int error_state;
    FootstepState::Ptr projected = projectFootstep(start_state_);
    if (global_transition_limit_) {
      if (!global_transition_limit_->check(zero_state_, projected)) {
        return false;
      }
    }
    if (projected) {
      start_state_ = projected;
      return true;
    }
    return false;
  }

  // double FootstepGraph::get_safety_cost(
  //   StatePtr to, cv_bridge::CvImage::Ptr label_image, sensor_msgs::CameraInfo::Ptr label_info)
  // {
  double FootstepGraph::get_safety_cost(
    Eigen::Vector3f original, cv_bridge::CvImage::Ptr label_image, sensor_msgs::CameraInfo::Ptr label_info)
  {
    tf::StampedTransform transform;
    try {
      // coords transform from "map" to "static_virtual_camera"
      listener.lookupTransform("map", "static_virtual_camera", ros::Time(0), transform);
      // Eigen::Affine3f to_pose_ = to->getPose();
      tf::Vector3 original_coords;
      // original_coords.setValue(to_pose_.translation()[0],
      //                          to_pose_.translation()[1],
      //                          to_pose_.translation()[2]);
      original_coords.setValue(original.x(),
                               original.y(),
                               original.z());
      tf::Vector3 translation_vector = transform.getOrigin();
      tf::Matrix3x3 rotation_matrix = transform.getBasis();
      tf::Vector3 target_coords;
      target_coords = rotation_matrix.inverse() * (original_coords - translation_vector);

      // calculate fov from camera info
      float fx = label_info->K[0];
      float fy = label_info->K[4];
      int image_width  = label_info->width;
      int image_height = label_info->height;
      double fovx = 2 * std::atan(image_width /(2*fx));
      double fovy = 2 * std::atan(image_height/(2*fy));

      // convert target coords to image pixel
      double fov_width  = 2 * target_coords.z() * std::tan(fovx/2);
      double fov_height = 2 * target_coords.z() * std::tan(fovy/2);
      int center_u = (target_coords.x() + fov_width /2) / fov_width  * image_width;
      int center_v = (target_coords.y() + fov_height/2) / fov_height * image_height;

      // get maximum of footfold safety
      float foot_size = 0.26; // should be modifid
      int radius = (foot_size / fov_width * image_width) / 2;
      int left_u   = std::max(center_u - radius    , 0           );
      int right_u  = std::min(center_u + radius + 1, image_width );
      int top_v    = std::max(center_v - radius    , 0           );
      int bottom_v = std::min(center_v + radius + 1, image_height);
      int max_cost = 0;
      for (int i=top_v; i<bottom_v; i++) {
        for (int j=left_u; j<right_u; j++) {
          if (int(label_image->image.data[i * image_width + j]) > max_cost) {
            max_cost = int(label_image->image.data[(i * image_width + j)]);
          }
        }
      }
      double safety_cost = double(max_cost);

      return safety_cost;
    }
    catch (tf::TransformException ex){
      ROS_ERROR("%s",ex.what());
      ros::Duration(1.0).sleep();
      return 0;
    }
  }

  double footstepHeuristicZero(
    SolverNode<FootstepState, FootstepGraph>::Ptr node, FootstepGraph::Ptr graph)
  {
    // best-first search
    return 0;
  }

  double footstepHeuristicStraight(
    SolverNode<FootstepState, FootstepGraph>::Ptr node, FootstepGraph::Ptr graph)
  {
    // distance_to_goal / maxSuccessor
    FootstepState::Ptr state = node->getState();
    FootstepState::Ptr goal = graph->getGoal(state->getLeg());
    Eigen::Vector3f state_pos(state->getPose().translation());
    Eigen::Vector3f goal_pos(goal->getPose().translation());
    return (std::abs((state_pos - goal_pos).norm() / graph->maxSuccessorDistance()));
  }

  double footstepHeuristicStraightRotation(
    SolverNode<FootstepState, FootstepGraph>::Ptr node, FootstepGraph::Ptr graph)
  {
    // distance_to_goal / maxSuccessor.trans + rotation_to_goal / maxSuccessor.rot
    FootstepState::Ptr state = node->getState();
    FootstepState::Ptr goal = graph->getGoal(state->getLeg());
    Eigen::Affine3f transform = state->getPose().inverse() * goal->getPose();
    return (std::abs(transform.translation().norm() / graph->maxSuccessorDistance()) +
               std::abs(Eigen::AngleAxisf(transform.rotation()).angle()) / graph->maxSuccessorRotation());
  }

  double footstepHeuristicStepCost(
    SolverNode<FootstepState, FootstepGraph>::Ptr node, FootstepGraph::Ptr graph,
    double first_rotation_weight,
    double second_rotation_weight)
  {
    FootstepState::Ptr state = node->getState();
    FootstepState::Ptr goal = graph->getGoal(state->getLeg());
    Eigen::Vector3f goal_pos(goal->getPose().translation());
    Eigen::Vector3f diff_pos(goal_pos - state->getPose().translation());
    diff_pos[2] = 0.0;          // Ignore z distance
    Eigen::Quaternionf first_rot;
    // Eigen::Affine3f::rotation is too slow because it calls SVD decomposition
    first_rot.setFromTwoVectors(state->getPose().matrix().block<3, 3>(0, 0) * Eigen::Vector3f::UnitX(),
                                diff_pos.normalized());

    Eigen::Quaternionf second_rot;
    second_rot.setFromTwoVectors(diff_pos.normalized(),
                                 goal->getPose().matrix().block<3, 3>(0, 0) * Eigen::Vector3f::UnitX());
    // is it correct??
    double first_theta = acos(first_rot.w()) * 2;
    double second_theta = acos(second_rot.w()) * 2;
    if (isnan(first_theta)) {
      first_theta = 0;
    }
    if (isnan(second_theta)) {
      second_theta = 0;
    }
    // acos := [0, M_PI]
    if (first_theta > M_PI) {
      first_theta = 2.0 * M_PI - first_theta;
    }
    if (second_theta > M_PI) {
      second_theta = 2.0 * M_PI - second_theta;
    }
    //return (Eigen::Vector2f(diff_pos[0], diff_pos[1]).norm() / graph->maxSuccessorDistance()) + std::abs(diff_pos[2]) / 0.2 +
    return (diff_pos.norm() / graph->maxSuccessorDistance()) + std::abs(diff_pos[2]) / 0.2 +
      (first_theta * first_rotation_weight + second_theta * second_rotation_weight) / graph->maxSuccessorRotation();
  }

  double footstepHeuristicFollowPathLine(
    SolverNode<FootstepState, FootstepGraph>::Ptr node, FootstepGraph::Ptr graph)
  {
    FootstepState::Ptr state = node->getState();
    FootstepState::Ptr goal = graph->getGoal(state->getLeg());

    Eigen::Vector3f goal_pos(goal->getPose().translation());
    Eigen::Vector3f state_pos(state->getPose().translation());
    Eigen::Vector3f state_mid_pos;
    if (state->getLeg() == jsk_footstep_msgs::Footstep::LEFT) {
      Eigen::Vector3f p(0, -0.1, 0);
      state_mid_pos = state->getPose() * p;
    } else { // Right
      Eigen::Vector3f p(0, 0.1, 0);
      state_mid_pos = state->getPose() * p;
    }
    double dist, to_goal, alp;
    int idx;
    Eigen::Vector3f foot;
    dist = graph->heuristic_path_->distanceWithInfo(state_mid_pos, foot, to_goal, idx, alp);

    //jsk_recognition_utils::FiniteLine::Ptr ln = graph->heuristic_path_->at(idx);
    Eigen::Vector3f dir = graph->heuristic_path_->getDirection(idx);

    Eigen::Quaternionf path_foot_rot;
    path_foot_rot.setFromTwoVectors(state->getPose().matrix().block<3, 3>(0, 0) * Eigen::Vector3f::UnitX(),
                                    dir);
    double path_foot_theta = acos(path_foot_rot.w()) * 2;
    if (path_foot_theta > M_PI) {
      path_foot_theta = 2.0 * M_PI - path_foot_theta;
      // foot_theta : [0, M_PI]
    }

    double step_cost = to_goal / graph->maxSuccessorDistance();
    double follow_cost = dist / 0.02; // ???
    double path_foot_rot_cost = path_foot_theta / graph->maxSuccessorRotation();

    Eigen::Vector3f goal_diff = goal_pos - state_pos;
    Eigen::Quaternionf goal_foot_rot;
    goal_foot_rot.setFromTwoVectors(state->getPose().matrix().block<3, 3>(0, 0) * Eigen::Vector3f::UnitX(),
                                    goal->getPose().matrix().block<3, 3>(0, 0) * Eigen::Vector3f::UnitX());
    double goal_foot_theta = acos(goal_foot_rot.w()) * 2;
    if (goal_foot_theta > M_PI) {
      goal_foot_theta = 2.0 * M_PI - goal_foot_theta;
    }
    double goal_foot_rot_cost = goal_foot_theta / graph->maxSuccessorRotation();

    //return step_cost + follow_cost + (4.0 * goal_foot_rot_cost) + (0.5 * path_foot_rot_cost);
    return 2*(step_cost + follow_cost + (0.5 * path_foot_rot_cost));
  }

  double footstepHeuristicStepSafety(
    SolverNode<FootstepState, FootstepGraph>::Ptr node, FootstepGraph::Ptr graph)
  {

    return 0;
  }
}
