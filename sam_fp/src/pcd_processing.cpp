#include "pcd_processing/pcd_processing.h"

// Initialize method
bool pcd_processing::initialize(ros::NodeHandle &nh) {
  // ROS_INFO_STREAM(pcd_processing::pointcloud_topic);

  // Initialize ROS subscribers, publishers, and other members
  point_cloud_sub_ =
      nh.subscribe(pointcloud_topic, 1, &pcd_processing::cloudCallback, this);
  masks_sub_ =
      nh.subscribe("/sam_mask", 1, &pcd_processing::masksCallback, this);
  objects_cloud_pub_ =
      nh.advertise<sensor_msgs::PointCloud2>("/objects_cloud", 1);
  object_boxes_pub_ =
      nh.advertise<visualization_msgs::MarkerArray>("/object_boxes", 1);
  // Initialize pointers
  raw_cloud_.reset(new cloud);
  preprocessed_cloud_.reset(new cloud);
  objects_cloud_.reset(new cloud);
  latest_maskID_msg_.reset(new masks_msgs::maskID);
  return true;  // Return true if initialization is successful
}

// Update method
void pcd_processing::update(const ros::Time &time) {
  // Update the pcd_processing object

  if (is_cloud_updated) {
    // Preprocess the raw cloud
    if (!raw_cloud_preprocessing(raw_cloud_, preprocessed_cloud_)) {
      ROS_ERROR("Raw cloud preprocessing failed!");
      return;
    }

    // Cut the preprocessed cloud //TODO: pass the argument
    if (!cut_point_cloud(preprocessed_cloud_, processed_masks_,
                         objects_cloud_)) {
      ROS_ERROR("Cutting point cloud failed!");
      return;
    };

    if (!extract_bboxes(objects_cloud_)) {
      ROS_ERROR("Extracting bounding boxes failed!");
      return;
    };

    // Publish the objects cloud
    pcl::toROSMsg(*objects_cloud_, cloudmsg_);
    ROS_INFO_STREAM("raw_cloud_:");
    ROS_INFO_STREAM(*raw_cloud_);
    ROS_INFO_STREAM("objects_cloud_:");
    ROS_INFO_STREAM(*objects_cloud_);
    objects_cloud_pub_.publish(cloudmsg_);
    ROS_INFO_STREAM("object_boxes_:");
    ROS_INFO_STREAM(object_boxes_);
    object_boxes_pub_.publish(object_boxes_);

    // Reset the flag
    is_cloud_updated = false;
  }
}

// Raw cloud preprocessing
bool pcd_processing::raw_cloud_preprocessing(cloudPtr &input,
                                             cloudPtr &output) {
  // Modify if further preprocessing needed
  // Note: Since the point cloud segmentation is pixel-wise, preprocessing may
  // cause lack of points.

  *output = *input;

  return true;  // Return true on success
}

// Cut point cloud
bool pcd_processing::cut_point_cloud(cloudPtr &input,
                                     const std::vector<singlemask> &masks,
                                     cloudPtr &objects) {
  // Implement the logic to cut the point cloud using masks
  // Point Cloud frame_id: camera_color_optical_frame
  // image_raw frame_id: camera_color_optical_frame
  // masks frame_id: camera_color_optical_frame

  // Clear the output cloud
  *objects = *input;
  objects->points.clear();

  // Iterate over each mask
  ROS_INFO_STREAM("masks:");
  ROS_INFO_STREAM(masks.size());
  for (const auto &mask : masks) {
    // Find the bounding box of the mask
    int min_x = mask.bbox[0];
    int min_y = mask.bbox[1];
    int width = mask.bbox[2];
    int height = mask.bbox[3];

    int number_of_ones = pcd_processing::countOnes(mask.segmentation);
    ROS_INFO_STREAM("number_of_ones:");
    ROS_INFO_STREAM(number_of_ones);

    int cols = mask.segmentation.cols();
    // Iterate over the points in the bounding box
    for (int i = min_y; i < min_y + height; ++i) {
      for (int j = min_x; j < min_x + width; ++j) {
        // Check if the mask includes this point
        if (mask.segmentation(i, j) == 1) {
          // Calculate the index in the point cloud
          int index = i * cols + j;
          if (index < input->points.size()) {
            // Add the point to the output cloud
            objects->points.push_back(input->points[index]);
          }
        }
      }
    }
  }
  objects->width = objects->points.size();
  objects->height = 1;  // Setting height to 1 implies the cloud is unorganized
  objects->is_dense =
      false;  // Set to false if there might be NaN or invalid points

  // Remove NaN points from the processed cloud
  std::vector<int> indices;
  pcl::removeNaNFromPointCloud(*objects, *objects, indices);

  return true;
}

bool pcd_processing::extract_bboxes(cloudPtr &input) {
  // Implement the logic to extract bounding boxes from the point cloud
  // homebrew method

  // Compute centroid and center
  Eigen::Vector4f centroid;
  point min_pt, max_pt;
  pcl::compute3DCentroid(*input, centroid);
  pcl::getMinMax3D(*input, min_pt, max_pt);
  Eigen::Vector3f center =
      (max_pt.getVector3fMap() + min_pt.getVector3fMap()) / 2;

  // Compute principal directions
  Eigen::Matrix3f covariance;
  pcl::computeCovarianceMatrixNormalized(*input, centroid, covariance);
  Eigen::SelfAdjointEigenSolver<Eigen::Matrix3f> eigen_solver(
      covariance, Eigen::ComputeEigenvectors);
  Eigen::Matrix3f eigenVectorsPCA = eigen_solver.eigenvectors();
  // Eigen::Vector3f eigenValuesPCA = eigen_solver.eigenvalues();
  eigenVectorsPCA.col(2) = eigenVectorsPCA.col(0).cross(eigenVectorsPCA.col(1));
  eigenVectorsPCA.col(0) = eigenVectorsPCA.col(1).cross(eigenVectorsPCA.col(2));
  eigenVectorsPCA.col(1) = eigenVectorsPCA.col(2).cross(eigenVectorsPCA.col(0));
  // Reorder eigenvectors based on eigenvalues (largest to smallest)
  eigenVectorsPCA.col(0).swap(eigenVectorsPCA.col(2));

  // Calculate transform matrix
  Eigen::Vector3f ea =
      (eigenVectorsPCA).eulerAngles(2, 1, 0);  // yaw pitch roll
  Eigen::AngleAxisf rollAngle(ea[1], Eigen::Vector3f::UnitZ());
  Eigen::Affine3f transform = Eigen::Affine3f::Identity();
  transform.translate(center);
  transform.rotate(rollAngle);

  // Calculate bounding box
  cloudPtr transformed_input(new cloud);
  pcl::transformPointCloud(*input, *transformed_input, transform.inverse());
  pcl::getMinMax3D(*transformed_input, min_pt, max_pt);
  center = (max_pt.getVector3fMap() + min_pt.getVector3fMap()) / 2;
  Eigen::Vector3f bbox = (max_pt.getVector3fMap() - min_pt.getVector3fMap());
  Eigen::Affine3f transform2 = Eigen::Affine3f::Identity();
  transform2.translate(center);
  Eigen::Affine3f transform3 = transform * transform2;

  // Publish bounding box
  visualization_msgs::Marker marker;
  marker.header.frame_id = "camera_color_optical_frame";
  marker.header.stamp = ros::Time::now();
  marker.ns = "bounding_box";
  marker.type = visualization_msgs::Marker::CUBE;
  marker.action = visualization_msgs::Marker::ADD;
  marker.pose.position.x = transform3.translation().x();
  marker.pose.position.y = transform3.translation().y();
  marker.pose.position.z = transform3.translation().z();
  // Quaternion
  marker.pose.orientation.y = sin(rollAngle.angle() / 2.0);
  marker.pose.orientation.w = cos(rollAngle.angle() / 2.0);
  marker.scale.x = bbox.x();
  marker.scale.y = bbox.y();
  marker.scale.z = bbox.z();
  marker.color.r = 1.0f;
  marker.color.g = 0.0f;
  marker.color.b = 0.0f;
  marker.color.a = 0.5;
  object_boxes_.markers.push_back(marker);

  // library method
  // auto config = YAML::LoadFile(ROOT_PATH "/config/config.yaml")["Cluster"];
  // using ClusterFactory = Factory<pc_utils::Cluster<PXYZ>, const YAML::Node
  // &>; if (auto filter = ClusterFactory::BuildT<std::shared_ptr>(
  //         pc_utils::ns("EuclideanCluster"), config["EuclideanCluster"]);
  //     filter) {
  //   std::vector<int> id, label;
  //   filter->extract(points, label, id);
  //   std::cout << points->size() << ", label: " << label.size()
  //             << ", id: " << id.size() << std::endl;
  // } else
  //   return false;
  // using BoundFactory = Factory<pc_utils::BoundingExtractor<point>>;
  // if (auto box_extractor = BoundFactory::BuildT<std::shared_ptr>(
  //         pc_utils::ns("OrientedBBox2p5DExtractor"));
  //     box_extractor) {
  //   BoundingBox box;
  //   box_extractor->extract(input, box);
  //   std::cout << box.pose.translation().transpose() << ", "
  //             << box.dxyz.transpose() << std::endl;
  //   return true;
  // } else
  //   return false
  return true;
}

// Cloud callback
void pcd_processing::cloudCallback(
    const sensor_msgs::PointCloud2ConstPtr &msg) {
  is_cloud_updated = true;

  pcl::fromROSMsg(*msg, *raw_cloud_);
}

// Masks callback
void pcd_processing::masksCallback(const masks_msgs::maskID::Ptr &msg) {
  // process new recieved masks
  processed_masks_ = maskID_msg_processing(msg);
}

std::vector<pcd_processing::singlemask> pcd_processing::maskID_msg_processing(
    const masks_msgs::maskID::Ptr &maskID) {
  ROS_INFO("mask_msg_preprocessing is triggered.");

  std::vector<singlemask> masks;
  for (const auto &singlemask_msg : maskID->maskID) {
    singlemask mask;
    mask.segmentation =
        Eigen::Map<const Eigen::Matrix<int64_t, Eigen::Dynamic, Eigen::Dynamic,
                                       Eigen::RowMajor>>(
            singlemask_msg.segmentation.data(), singlemask_msg.shape[0],
            singlemask_msg.shape[1]);
    mask.area = singlemask_msg.area;
    mask.bbox = singlemask_msg.bbox;
    mask.predicted_iou = singlemask_msg.predicted_iou;
    mask.point_coords = Eigen::Map<
        const Eigen::Matrix<float, Eigen::Dynamic, 2, Eigen::RowMajor>>(
        singlemask_msg.point_coords.data(),
        singlemask_msg.point_coords.size() / 2, 2);
    mask.stability_score = singlemask_msg.stability_score;
    mask.crop_box = singlemask_msg.crop_box;

    masks.push_back(mask);
  }

  ROS_INFO_STREAM("length of masks before erase:");
  ROS_INFO_STREAM(masks.size());

  // Sort the masks by area
  auto compareArea = [](const singlemask &a, const singlemask &b) {
    return a.area < b.area;
  };
  std::sort(masks.begin(), masks.end(), compareArea);
  // Erase the masks with the largest area (the background mask)
  if (masks.size() > 5) {
    masks.erase(masks.end() - 5, masks.end());
  }

  return masks;
}

int pcd_processing::countOnes(
    const Eigen::Matrix<int64_t, Eigen::Dynamic, Eigen::Dynamic,
                        Eigen::RowMajor> &matrix) {
  int count = 0;
  for (int i = 0; i < matrix.rows(); i++) {
    for (int j = 0; j < matrix.cols(); j++) {
      if (matrix(i, j) == 1) {
        count++;
      }
    }
  }
  return count;
}
