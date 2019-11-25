/*******************************************************
 * Copyright (C) 2019, Aerial Robotics Group, Hong Kong University of Science and Technology
 *
 * This file is part of VINS.
 *
 * Licensed under the GNU General Public License v3.0;
 * you may not use this file except in compliance with the License.
 *******************************************************/

#include "estimator.h"
#include "../utility/visualization.h"

using namespace common;

void CRSMatrix2EigenMatrix(const ceres::CRSMatrix &crs_matrix, Eigen::MatrixXd &eigen_matrix)
{
    eigen_matrix = Eigen::MatrixXd::Zero(crs_matrix.num_rows, crs_matrix.num_cols);
    for (int row = 0; row < crs_matrix.num_rows; row++)
    {
        int start = crs_matrix.rows[row];
        int end = crs_matrix.rows[row + 1] - 1;
        for (int i = start; i <= end; i++)
        {
            int col = crs_matrix.cols[i];
            double value = crs_matrix.values[i];
            eigen_matrix(row, col) = value;
        }
    }
}


Estimator::Estimator()
{
    ROS_INFO("init begins");
    init_thread_flag_ = false;
    clearState();
}

Estimator::~Estimator()
{
    if (MULTIPLE_THREAD)
    {
        process_thread_.join();
        printf("join thread \n");
    }
}

void Estimator::clearState()
{
    printf("[estimator] clear state\n");
    m_process_.lock();

    b_system_inited_ = false;

    prev_time_ = -1;
    cur_time_ = 0;
    input_cloud_cnt_ = 0;

    td_ = 0;

    solver_flag_ = INITIAL;

    pose_laser_cur_.clear();
    // pose_prev_cur_.clear();
    pose_rlt_.clear();

    qbl_.clear();
    tbl_.clear();
    tdbl_.clear();

    initial_extrinsics_.clearState();

    ini_fixed_local_map_ = false;

    cir_buf_cnt_ = 0;

    Qs_.clear();
    Ts_.clear();
    Header_.clear();
    surf_points_stack_.clear();
    surf_points_stack_size_.clear();
    // corner_points_stack_.clear();
    // corner_points_stack_size_.clear();

    surf_points_local_map_.clear();
    surf_points_local_map_filtered_.clear();
    surf_points_pivot_map_.clear();
    corner_points_local_map_.clear();
    corner_points_local_map_filtered_.clear();
    corner_points_pivot_map_.clear();

    surf_map_features_.clear();
    corner_map_features_.clear();

    cumu_surf_map_features_.clear();
    cumu_corner_map_features_.clear();

    pose_local_.clear();

    last_marginalization_info_ = nullptr;

    m_process_.unlock();
}

void Estimator::setParameter()
{
    m_process_.lock();

    pose_laser_cur_.resize(NUM_OF_LASER);
    // pose_prev_cur_.resize(NUM_OF_LASER);
    pose_rlt_.resize(NUM_OF_LASER);

    qbl_.resize(NUM_OF_LASER);
    tbl_.resize(NUM_OF_LASER);
    tdbl_.resize(NUM_OF_LASER);
    for (int i = 0; i < NUM_OF_LASER; i++)
    {
        qbl_[i] = QBL[i];
        tbl_[i] = TBL[i];
        tdbl_[i] = TDBL[i];
        cout << "Given extrinsic Laser_" << i << ": " << Pose(QBL[i], TBL[i], TDBL[i]) << endl;
    }

    initial_extrinsics_.setParameter();

    Qs_.resize(WINDOW_SIZE + 1);
    Ts_.resize(WINDOW_SIZE + 1);
    Header_.resize(WINDOW_SIZE + 1);
    surf_points_stack_.resize(NUM_OF_LASER);
    surf_points_stack_size_.resize(NUM_OF_LASER);
    corner_points_stack_.resize(NUM_OF_LASER);
    corner_points_stack_size_.resize(NUM_OF_LASER);

    pose_local_.resize(NUM_OF_LASER);
    for (int i = 0; i < NUM_OF_LASER; i++)
    {
        surf_points_stack_[i].resize(WINDOW_SIZE + 1);
        surf_points_stack_size_[i].resize(WINDOW_SIZE + 1);
        corner_points_stack_[i].resize(WINDOW_SIZE + 1);
        corner_points_stack_size_[i].resize(WINDOW_SIZE + 1);
        pose_local_[i].resize(WINDOW_SIZE + 1);
    }

    surf_points_local_map_.resize(NUM_OF_LASER);
    surf_points_local_map_filtered_.resize(NUM_OF_LASER);
    surf_points_pivot_map_.resize(NUM_OF_LASER);
    corner_points_local_map_.resize(NUM_OF_LASER);
    corner_points_local_map_filtered_.resize(NUM_OF_LASER);
    corner_points_pivot_map_.resize(NUM_OF_LASER);

    cumu_surf_map_features_.resize(NUM_OF_LASER);
    cumu_corner_map_features_.resize(NUM_OF_LASER);

    printf("MULTIPLE_THREAD is %d\n", MULTIPLE_THREAD);
    if (MULTIPLE_THREAD && !init_thread_flag_)
    {
        init_thread_flag_ = true;
        process_thread_ = std::thread(&Estimator::processMeasurements, this);
    }

    para_pose_ = new double *[OPT_WINDOW_SIZE + 1];
    for (int i = 0; i < OPT_WINDOW_SIZE + 1; i++)
    {
        para_pose_[i] = new double[SIZE_POSE];
    }
    para_ex_pose_ = new double *[NUM_OF_LASER];
    for (int i = 0; i < NUM_OF_LASER; i++)
    {
        para_ex_pose_[i] = new double[SIZE_POSE];
    }
    para_td_ = new double[NUM_OF_LASER];

    eig_thre_calib_ = std::vector<double>(OPT_WINDOW_SIZE + NUM_OF_LASER + 1, EIG_INITIAL);

    m_process_.unlock();
}

void Estimator::changeSensorType(int use_imu, int use_stereo)
{
    bool restart = false;
    m_process_.lock();
    m_process_.unlock();
    if(restart)
    {
        clearState();
        setParameter();
    }
}

void Estimator::inputCloud(const double &t, const std::vector<PointCloud> &v_laser_cloud_in)
{
    input_cloud_cnt_++;
    std::vector<cloudFeature> feature_frame;
    TicToc feature_ext_time;
    for (int i = 0; i < v_laser_cloud_in.size(); i++)
    {
        // printf("[LASER %u]: \n", i);
        feature_frame.push_back(f_extract_.extractCloud(t, v_laser_cloud_in[i]));
    }
    printf("featureExt time: %fms \n", feature_ext_time.toc());

    m_buf_.lock();
    feature_buf_.push(make_pair(t, feature_frame));
    m_buf_.unlock();
    if (!MULTIPLE_THREAD) processMeasurements();
}

void Estimator::inputCloud(const double &t, const PointCloud &laser_cloud_in)
{
    input_cloud_cnt_++;
    std::vector<cloudFeature> feature_frame;
    TicToc feature_ext_time;
    // printf("LASER 0: \n");
    feature_frame.push_back(f_extract_.extractCloud(t, laser_cloud_in));
    printf("featureExt time: %fms \n", feature_ext_time.toc());

    m_buf_.lock();
    feature_buf_.push(make_pair(t, feature_frame));
    m_buf_.unlock();
    if (!MULTIPLE_THREAD) processMeasurements();
}

void Estimator::processMeasurements()
{
    while (1)
    {
        if (!feature_buf_.empty())
        {
            TicToc t_process;
            cur_feature_ = feature_buf_.front();
            cur_time_ = cur_feature_.first + td_;
            assert(cur_feature_.second.size() == NUM_OF_LASER);

            m_buf_.lock();
            feature_buf_.pop();
            m_buf_.unlock();

            m_process_.lock();
            process();

            printStatistics(*this, 0);
            pubPointCloud(*this, cur_time_);
            pubOdometry(*this, cur_time_);
            m_process_.unlock();

            ROS_WARN("frame: %d, processMea time: %fms\n", input_cloud_cnt_, t_process.toc());
        }
        if (!MULTIPLE_THREAD) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void Estimator::process()
{
    if (!b_system_inited_)
    {
        b_system_inited_ = true;
        printf("System initialization finished \n");
        for (int i = 0; i < NUM_OF_LASER; i++)
        {
            pose_rlt_[i] = Pose();
            pose_laser_cur_[i] = Pose();
        }
    } else
    {
        TicToc t_mloam_tracker;
        // -----------------
        // tracker and initialization
        if (ESTIMATE_EXTRINSIC == 2)
        {
            // feature tracker: estimate the relative transformations of each lidar
            for (int i = 0; i < NUM_OF_LASER; i++)
            {
                printf("[LASER %d]:\n", i);
                cloudFeature &cur_cloud_feature = cur_feature_.second[i];
                cloudFeature &prev_cloud_feature = prev_feature_.second[i];
                pose_rlt_[i] = lidar_tracker_.trackCloud(prev_cloud_feature, cur_cloud_feature, pose_rlt_[i]);
                pose_laser_cur_[i] = pose_laser_cur_[i] * pose_rlt_[i];
                std::cout << "relative transform: " << pose_rlt_[i] << std::endl;
                std::cout << "current transform: " << pose_laser_cur_[i] << std::endl;
            }
            printf("mloam_tracker %fms\n", t_mloam_tracker.toc());

            // initialize extrinsics
            for (int i = 0; i < NUM_OF_LASER; i++) initial_extrinsics_.addPose(pose_rlt_[i], i);
            if (cir_buf_cnt_ == WINDOW_SIZE)
            {
                TicToc t_calib_ext;
                printf("calibrating extrinsic param\n");
                printf("sufficient movement is needed \n");
                for (int i = 0; i < NUM_OF_LASER; i++)
                {
                    Pose calib_result;
                    // if (IDX_REF == i) continue;
                    if ((initial_extrinsics_.cov_rot_state_[i]) || (initial_extrinsics_.calibExRotation(IDX_REF, i, calib_result)))
                    {
                        printf("sufficient translation movement is needed\n");
                        initial_extrinsics_.setCovRotation(i);
                        if ((initial_extrinsics_.cov_pos_state_[i]) || (initial_extrinsics_.calibExTranslation(IDX_REF, i, calib_result)))
                        {
                            initial_extrinsics_.setCovTranslation(i);
                            ROS_WARN_STREAM("number of pose: " << initial_extrinsics_.frame_cnt_);
                            ROS_WARN_STREAM("initial extrinsic of laser_" << i << ": " << calib_result);
                            qbl_[i] = calib_result.q_;
                            tbl_[i] = calib_result.t_;
                            // tdbl_[i] = calib_result.td_;
                            QBL[i] = calib_result.q_;
                            TBL[i] = calib_result.t_;
                            // TDBL[i] = calib_result.td_;
                        }
                    }
                }
                if ((initial_extrinsics_.full_cov_rot_state_) && (initial_extrinsics_.full_cov_pos_state_))
                {
                    ROS_WARN("all initial extrinsic rotation calib success");
                    ESTIMATE_EXTRINSIC = 1;
                    initial_extrinsics_.saveStatistics();
                }
                printf("whole initialize extrinsics %fms\n", t_calib_ext.toc());
            }
        }
        // tracker
        else if (ESTIMATE_EXTRINSIC != 2)
        {
            cloudFeature &cur_cloud_feature = cur_feature_.second[IDX_REF];
            cloudFeature &prev_cloud_feature = prev_feature_.second[IDX_REF];
            pose_rlt_[IDX_REF] = lidar_tracker_.trackCloud(prev_cloud_feature, cur_cloud_feature, pose_rlt_[IDX_REF]);
            pose_laser_cur_[IDX_REF] = Pose(Qs_[cir_buf_cnt_-1], Ts_[cir_buf_cnt_-1]) * pose_rlt_[IDX_REF];
            std::cout << "relative transform: " << pose_rlt_[IDX_REF] << std::endl;
            std::cout << "current transform: " << pose_laser_cur_[IDX_REF] << std::endl;
            // Eigen::Vector3d ea = pose_rlt_[IDX_REF].T_.topLeftCorner<3, 3>().eulerAngles(2, 1, 0);
            // printf("relative euler (deg): %f, %f, %f\n", toDeg(ea(0)), toDeg(ea(1)), toDeg(ea(2)));
            printf("mloam_tracker %fms\n", t_mloam_tracker.toc());
        }
    }

    // get newest pose
    Qs_[cir_buf_cnt_] = pose_laser_cur_[IDX_REF].q_;
    Ts_[cir_buf_cnt_] = pose_laser_cur_[IDX_REF].t_;

    // get newest point cloud
    Header_[cir_buf_cnt_].stamp = ros::Time(cur_feature_.first);
    PointICloud cloud_downsampled_;
    for (int n = 0; n < NUM_OF_LASER; n++)
    {
        PointICloud &corner_points = cur_feature_.second[n]["corner_points_less_sharp"];
        f_extract_.down_size_filter_corner_.setInputCloud(boost::make_shared<PointICloud>(corner_points));
        f_extract_.down_size_filter_corner_.filter(cloud_downsampled_);
        corner_points_stack_[n][cir_buf_cnt_] = cloud_downsampled_;
        corner_points_stack_size_[n][cir_buf_cnt_] = cloud_downsampled_.size();

        PointICloud &surf_points = cur_feature_.second[n]["surf_points_less_flat"];
        f_extract_.down_size_filter_surf_.setInputCloud(boost::make_shared<PointICloud>(surf_points));
        f_extract_.down_size_filter_surf_.filter(cloud_downsampled_);
        surf_points_stack_[n][cir_buf_cnt_] = cloud_downsampled_;
        surf_points_stack_size_[n][cir_buf_cnt_] = cloud_downsampled_.size();
    }
    // printSlideWindow();

    // -----------------
    switch (solver_flag_)
    {
        // INITIAL: multi-LiDAR individual tracking
        case INITIAL:
        {
            printf("[INITIAL]\n");
            slideWindow();
            if (cir_buf_cnt_ < WINDOW_SIZE)
            {
                cir_buf_cnt_++;
                if (cir_buf_cnt_ == WINDOW_SIZE)
                {
                    slideWindow(); // TODO: a bug in the buffer push() function which needs to be fixed
                }
            }
            if ((cir_buf_cnt_ == WINDOW_SIZE) && (ESTIMATE_EXTRINSIC != 2))
            {
                solver_flag_ = NON_LINEAR;
            }
            break;
        }
        // NON_LINEAR: single LiDAR tracking and perform scan-to-map constrains
        case NON_LINEAR:
        {
            // local optimization: optimize the relative LiDAR measurments
            printf("[NON_LINEAR]\n");
            optimizeMap();
            slideWindow();
            if (ESTIMATE_EXTRINSIC) evalCalib();
            break;
        }
    }

    // swap features
    prev_time_ = cur_time_;
    prev_feature_.first = prev_time_;
    prev_feature_.second.clear();
    for (int i = 0; i < cur_feature_.second.size(); i++)
    {
        cloudFeature tmp_cloud_feature;
        for (auto iter = cur_feature_.second[i].begin(); iter != cur_feature_.second[i].end(); iter++)
        {
            if (iter->first.find("less") != std::string::npos)
                tmp_cloud_feature.insert(make_pair(iter->first, iter->second));
        }
        prev_feature_.second.push_back(tmp_cloud_feature);
    }
    // or
    // prev_feature_.second = cur_feature_.second;
}

// TODO: optimize_direct_calib
void Estimator::optimizeMap()
{
    TicToc t_prep_solver;
    int pivot_idx = WINDOW_SIZE - OPT_WINDOW_SIZE;

    // -----------------
    ceres::Problem problem;
    ceres::Solver::Summary summary;
    // ceres: set lossfunction and problem
    ceres::LossFunction *loss_function;
    loss_function = new ceres::HuberLoss(0.5);
    // loss_function = new ceres::CauchyLoss(1.0);
    // ceres: set options and solve the non-linear equation
    ceres::Solver::Options options;
    // options.linear_solver_type = ceres::DENSE_SCHUR;
    options.num_threads = 3;
    // options.trust_region_strategy_type = ceres::DOGLEG;
    options.max_num_iterations = NUM_ITERATIONS;
    //options.use_explicit_schur_complement = true;
    //options.minimizer_progress_to_stdout = true;
    //options.use_nonmonotonic_steps = true;
    options.max_solver_time_in_seconds = SOLVER_TIME;

    // ****************************************************
    // ceres: add parameter block
    vector2Double();
    // printParameter();
    // if (!OPTIMAL_ODOMETRY) printParameter();

    std::vector<double *> para_ids;
    std::vector<PoseLocalParameterization *> local_param_ids;
    for (int i = 0; i < OPT_WINDOW_SIZE + 1; i++)
    {
        PoseLocalParameterization *local_parameterization = new PoseLocalParameterization();
        problem.AddParameterBlock(para_pose_[i], SIZE_POSE, local_parameterization);
        local_param_ids.push_back(local_parameterization);
        para_ids.push_back(para_pose_[i]);
    }
    problem.SetParameterBlockConstant(para_pose_[0]);

    for (int i = 0; i < NUM_OF_LASER; i++)
    {
        PoseLocalParameterization *local_parameterization = new PoseLocalParameterization();
        problem.AddParameterBlock(para_ex_pose_[i], SIZE_POSE, local_parameterization);
        local_param_ids.push_back(local_parameterization);
        para_ids.push_back(para_ex_pose_[i]);
        if (!ESTIMATE_EXTRINSIC)
        {
            problem.SetParameterBlockConstant(para_ex_pose_[i]);
        }
    }
    problem.SetParameterBlockConstant(para_ex_pose_[IDX_REF]);

    // for (int i = 0; i < NUM_OF_LASER; i++)
    // {
    //     problem.AddParameterBlock(&para_td_[i], 1);
    //     para_ids.push_back(&para_td_[i]);
    //     if (!ESTIMATE_TD)
    //     {
    //         problem.SetParameterBlockConstant(&para_td_[i]);
    //     }
    // }
    // problem.SetParameterBlockConstant(&para_td_[IDX_REF]);

    // ****************************************************
    // ceres: add marginalization error of previous parameter blocks
    std::vector<ceres::internal::ResidualBlock *> res_ids_marg;
    if ((MARGINALIZATION_FACTOR) && (last_marginalization_info_))
    {
        MarginalizationFactor *marginalization_factor = new MarginalizationFactor(last_marginalization_info_);
        ceres::internal::ResidualBlock *res_id_marg = problem.AddResidualBlock(marginalization_factor, NULL, last_marginalization_parameter_blocks_);
        res_ids_marg.push_back(res_id_marg);
    }

    // ****************************************************
    // ceres: add residual block within the sliding window
    std::vector<ceres::internal::ResidualBlock *> res_ids_proj;
    if (PRIOR_FACTOR)
    {
        for (int n = 0; n < NUM_OF_LASER; n++)
        {
            PriorFactor *f = new PriorFactor(tbl_[n], qbl_[n], PRIOR_FACTOR_POS, PRIOR_FACTOR_ROT);
            ceres::internal::ResidualBlock *res_id = problem.AddResidualBlock(f, NULL, para_ex_pose_[n]);
            res_ids_proj.push_back(res_id);
        }
    }

    // TODO: focus on online calibration
    if (ESTIMATE_EXTRINSIC == 1)
    {
        ROS_WARN("Online Calibration");
        buildCalibMap();
        if (POINT_PLANE_FACTOR)
        {
            // CHECK_JACOBIAN = 0;
            for (int i = pivot_idx + 1; i < WINDOW_SIZE + 1; i++)
            {
                int n = IDX_REF;
                std::vector<PointPlaneFeature> &features_frame = surf_map_features_[n][i];
                // printf("Laser_%d, Win_%d, features: %d\n", n, i, features_frame.size());
                for (auto &feature: features_frame)
                {
                    const double &s = feature.score_;
                    const Eigen::Vector3d &p_data = feature.point_;
                    const Eigen::Vector4d &coeff_ref = feature.coeffs_;
                    LidarPivotPlaneNormFactor *f = new LidarPivotPlaneNormFactor(p_data, coeff_ref, s);
                    ceres::internal::ResidualBlock *res_id = problem.AddResidualBlock(f, loss_function,
                        para_pose_[0], para_pose_[i - pivot_idx], para_ex_pose_[n]);
                    res_ids_proj.push_back(res_id);
                    if (CHECK_JACOBIAN)
                    {
                        double **tmp_param = new double *[3];
                        tmp_param[0] = para_pose_[0];
                        tmp_param[1] = para_pose_[i - pivot_idx];
                        tmp_param[2] = para_ex_pose_[n];
                        f->check(tmp_param);
                        CHECK_JACOBIAN = 0;
                    }
                }
            }
            for (int n = 0; n < NUM_OF_LASER; n++) cumu_surf_map_features_[n].push_back(surf_map_features_[n][pivot_idx]);
            if (cumu_surf_map_features_[IDX_REF].size() == N_CUMU_FEATURE)
            {
                ROS_WARN("*************** Calibration");
                for (int n = 0; n < NUM_OF_LASER; n++)
                {
                    if (n == IDX_REF) continue;
                    for (auto &features_frame: cumu_surf_map_features_[n])
                    {
                        for (auto &feature: features_frame)
                        {
                            const double &s = feature.score_;
                            const Eigen::Vector3d &p_data = feature.point_;
                            const Eigen::Vector4d &coeff_ref = feature.coeffs_;
                            LidarPivotTargetPlaneNormFactor *f = new LidarPivotTargetPlaneNormFactor(p_data, coeff_ref, s, 1.0);
                            ceres::internal::ResidualBlock *res_id = problem.AddResidualBlock(f, loss_function, para_ex_pose_[n]);
                            res_ids_proj.push_back(res_id);
                        }
                    }
                }
                if (!MARGINALIZATION_FACTOR)
                {
                    cumu_surf_map_features_.clear();
                    cumu_surf_map_features_.resize(NUM_OF_LASER);
                }
            }
        }

        if (POINT_EDGE_FACTOR)
        {
            for (int n = 0; n < NUM_OF_LASER; n++) cumu_corner_map_features_[n].push_back(corner_map_features_[n][pivot_idx]);
            if (cumu_corner_map_features_[IDX_REF].size() == N_CUMU_FEATURE)
            {
                for (int n = 0; n < NUM_OF_LASER; n++)
                {
                    if (n == IDX_REF) continue;
                    for (auto &features_frame: cumu_corner_map_features_[n])
                    {
                        for (auto &feature: features_frame)
                        {
                            const double &s = feature.score_;
                            const Eigen::Vector3d &p_data = feature.point_;
                            const Eigen::Vector4d &coeff_ref = feature.coeffs_;
                            LidarPivotTargetPlaneNormFactor *f = new LidarPivotTargetPlaneNormFactor(p_data, coeff_ref, s, 1.0);
                            ceres::internal::ResidualBlock *res_id = problem.AddResidualBlock(f, loss_function, para_ex_pose_[n]);
                            res_ids_proj.push_back(res_id);
                        }
                    }
                }
                if (!MARGINALIZATION_FACTOR)
                {
                    cumu_corner_map_features_.clear();
                    cumu_corner_map_features_.resize(NUM_OF_LASER);
                }
            }
        }
    }
    // TODO: focus on online odometry estimation
    else if (ESTIMATE_EXTRINSIC == 0)
    {
        ROS_WARN("Multi-LiDAR Odometry");
        for (int n = 0; n < NUM_OF_LASER; n++)
        {
            problem.SetParameterBlockConstant(para_ex_pose_[n]);
            // problem.SetParameterBlockConstant(&para_td_[n]);
        }
        buildLocalMap();
        if (POINT_PLANE_FACTOR)
        {
            for (int n = 0; n < NUM_OF_LASER; n++)
            {
                for (int i = pivot_idx + 1; i < WINDOW_SIZE + 1; i++)
                {
                    std::vector<PointPlaneFeature> &features_frame = surf_map_features_[n][i];
                    // printf("Laser_%d, Win_%d, features: %d\n", n, i, features_frame.size());
                    for (auto &feature: features_frame)
                    {
                        const double &s = feature.score_;
                        const Eigen::Vector3d &p_data = feature.point_;
                        const Eigen::Vector4d &coeff_ref = feature.coeffs_;
                        LidarPivotPlaneNormFactor *f = new LidarPivotPlaneNormFactor(p_data, coeff_ref, s);
                        ceres::internal::ResidualBlock *res_id = problem.AddResidualBlock(f, loss_function,
                            para_pose_[0], para_pose_[i - pivot_idx], para_ex_pose_[n]);
                        res_ids_proj.push_back(res_id);
                    }
                }
            }
        }
    }

    // *******************************
    ROS_WARN("Before optimization");
    if (EVALUATE_RESIDUAL) evalResidual(problem, local_param_ids, para_ids, res_ids_proj, last_marginalization_info_, res_ids_marg, true);

    printf("prepare ceres %fms\n", t_prep_solver.toc()); // cost time
    if (!OPTIMAL_ODOMETRY) return;

    TicToc t_ceres_solver;
    ceres::Solve(options, &problem, &summary);
    std::cout << summary.BriefReport() << std::endl;
    // std::cout << summary.FullReport() << std::endl;
    printf("ceres solver costs: %fms\n", t_ceres_solver.toc());

    ROS_WARN("After optimization");
    if (EVALUATE_RESIDUAL) evalResidual(problem, local_param_ids, para_ids, res_ids_proj, last_marginalization_info_, res_ids_marg);

    double2Vector();
    // printParameter();

    // ****************************************************
    // ceres: marginalization of current parameter block
    if (MARGINALIZATION_FACTOR)
    {
        TicToc t_whole_marginalization;
        MarginalizationInfo *marginalization_info = new MarginalizationInfo();
        vector2Double();
        // indicate the marginalized parameter blocks
        if (last_marginalization_info_)
        {
            std::vector<int> drop_set;
            for (int i = 0; i < static_cast<int>(last_marginalization_parameter_blocks_.size()); i++)
            {
                // indicate the dropped pose to calculate the relatd residuals
                if (last_marginalization_parameter_blocks_[i] == para_pose_[0]) drop_set.push_back(i);
            }
            MarginalizationFactor *marginalization_factor = new MarginalizationFactor(last_marginalization_info_);
            ResidualBlockInfo *residual_block_info = new ResidualBlockInfo(marginalization_factor, NULL,
                last_marginalization_parameter_blocks_, drop_set);
            marginalization_info->addResidualBlockInfo(residual_block_info);
        }

        if (PRIOR_FACTOR)
        {
            for (int n = 0; n < NUM_OF_LASER; n++)
            {
                PriorFactor *f = new PriorFactor(tbl_[n], qbl_[n], PRIOR_FACTOR_POS, PRIOR_FACTOR_ROT);
                ResidualBlockInfo *residual_block_info = new ResidualBlockInfo(f, NULL,
                    std::vector<double *>{para_ex_pose_[n]}, std::vector<int>{});
                marginalization_info->addResidualBlockInfo(residual_block_info);
            }
        }

        // TODO: add marginalization block
        if (ESTIMATE_EXTRINSIC == 1)
        {
            if (POINT_PLANE_FACTOR)
            {
                for (int i = pivot_idx + 1; i < WINDOW_SIZE + 1; i++)
                {
                    int n = IDX_REF;
                    std::vector<PointPlaneFeature> &features_frame = surf_map_features_[n][i];
                    for (auto &feature: features_frame)
                    {
                        const double &s = feature.score_;
                        const Eigen::Vector3d &p_data = feature.point_;
                        const Eigen::Vector4d &coeff_ref = feature.coeffs_;
                        LidarPivotPlaneNormFactor *f = new LidarPivotPlaneNormFactor(p_data, coeff_ref, s);
                        ResidualBlockInfo *residual_block_info = new ResidualBlockInfo(f, loss_function,
                            std::vector<double *>{para_pose_[0], para_pose_[i - pivot_idx], para_ex_pose_[n]}, std::vector<int>{0});
                        marginalization_info->addResidualBlockInfo(residual_block_info);
                    }
                }

                if (cumu_surf_map_features_[IDX_REF].size() == N_CUMU_FEATURE)
                {
                    for (int n = 0; n < NUM_OF_LASER; n++)
                    {
                        if (n == IDX_REF) continue;
                        for (auto &features_frame: cumu_surf_map_features_[n])
                        {
                            for (auto &feature: features_frame)
                            {
                                const double &s = feature.score_;
                                const Eigen::Vector3d &p_data = feature.point_;
                                const Eigen::Vector4d &coeff_ref = feature.coeffs_;
                                LidarPivotTargetPlaneNormFactor *f = new LidarPivotTargetPlaneNormFactor(p_data, coeff_ref, s, 1.0);
                                ResidualBlockInfo *residual_block_info = new ResidualBlockInfo(f, loss_function,
                                    std::vector<double *>{para_ex_pose_[n]}, std::vector<int>{});
                                marginalization_info->addResidualBlockInfo(residual_block_info);
                            }
                        }
                    }
                    cumu_surf_map_features_.clear();
                    cumu_surf_map_features_.resize(NUM_OF_LASER);
                }
            }

            if (POINT_EDGE_FACTOR)
            {
                if (cumu_corner_map_features_[IDX_REF].size() == N_CUMU_FEATURE)
                {
                    for (int n = 0; n < NUM_OF_LASER; n++)
                    {
                        if (n == IDX_REF) continue;
                        for (auto &features_frame: cumu_corner_map_features_[n])
                        {
                            for (auto &feature: features_frame)
                            {
                                const double &s = feature.score_;
                                const Eigen::Vector3d &p_data = feature.point_;
                                const Eigen::Vector4d &coeff_ref = feature.coeffs_;
                                LidarPivotTargetPlaneNormFactor *f = new LidarPivotTargetPlaneNormFactor(p_data, coeff_ref, s, 1.0);
                                ResidualBlockInfo *residual_block_info = new ResidualBlockInfo(f, loss_function,
                                    std::vector<double *>{para_ex_pose_[n]}, std::vector<int>{});
                                marginalization_info->addResidualBlockInfo(residual_block_info);
                            }
                        }
                    }
                    cumu_corner_map_features_.clear();
                    cumu_corner_map_features_.resize(NUM_OF_LASER);
                }
            }
        }
        else if (ESTIMATE_EXTRINSIC == 0)
        {
            if (POINT_PLANE_FACTOR)
            {
                for (int n = 0; n < NUM_OF_LASER; n++)
                {
                    for (int i = pivot_idx + 1; i < WINDOW_SIZE + 1; i++)
                    {
                        std::vector<PointPlaneFeature> &features_frame = surf_map_features_[n][i];
                        for (auto &feature: features_frame)
                        {
                            const double &s = feature.score_;
                            const Eigen::Vector3d &p_data = feature.point_;
                            const Eigen::Vector4d &coeff_ref = feature.coeffs_;
                            LidarPivotPlaneNormFactor *f = new LidarPivotPlaneNormFactor(p_data, coeff_ref, s);
                            ResidualBlockInfo *residual_block_info = new ResidualBlockInfo(f, loss_function,
                                vector<double *>{para_pose_[0], para_pose_[i - pivot_idx], para_ex_pose_[n]}, std::vector<int>{0});
                            marginalization_info->addResidualBlockInfo(residual_block_info);
                        }
                    }
                }
            }
        }

        //! calculate the residuals and jacobian of all ResidualBlockInfo over the marginalized parameter blocks,
        //! for next iteration, the linearization point is assured and fixed
        //! adjust the memory of H and b to implement the Schur complement
        TicToc t_pre_margin;
        marginalization_info->preMarginalize();
        printf("pre marginalization: %fms\n", t_pre_margin.toc());

        TicToc t_margin;
        marginalization_info->marginalize();
        printf("marginalization: %fms\n", t_margin.toc());

        //! indicate shared memory of parameter blocks except for the dropped state
        std::unordered_map<long, double *> addr_shift;
        for (int i = pivot_idx + 1; i < WINDOW_SIZE + 1; i++)
        {
            addr_shift[reinterpret_cast<long>(para_pose_[i - pivot_idx])] = para_pose_[i - pivot_idx - 1];
        }
        for (int n = 0; n < NUM_OF_LASER; n++)
        {
            addr_shift[reinterpret_cast<long>(para_ex_pose_[n])] = para_ex_pose_[n];
        }
        // for (int n = 0; n < NUM_OF_LASER; n++)
        // {
        //     addr_shift[reinterpret_cast<long>(&para_td_[n])] = &para_td_[n];
        // }
        vector<double *> parameter_blocks = marginalization_info->getParameterBlocks(addr_shift);
        if (last_marginalization_info_)
        {
            delete last_marginalization_info_;
        }
        last_marginalization_info_ = marginalization_info;
        last_marginalization_parameter_blocks_ = parameter_blocks; // save parameter_blocks at the last optimization
        printf("whole marginalization costs: %fms\n", t_whole_marginalization.toc());
    }
}

/****************************************************************************************/
void Estimator::buildCalibMap()
{
    TicToc t_build_map;

    int pivot_idx = WINDOW_SIZE - OPT_WINDOW_SIZE;
    Pose pose_pivot(Qs_[pivot_idx], Ts_[pivot_idx]);
    // -----------------
    // build static local map using fixed poses
    PointICloud surf_points_trans, corner_points_trans;
    if (!ini_fixed_local_map_)
    {
        PointICloud surf_points_tmp, corner_points_tmp;
        Pose pose_ext = Pose(qbl_[IDX_REF], tbl_[IDX_REF]);
        for (int i = 0; i <= pivot_idx; i++)
        {
            Pose pose_i(Qs_[i], Ts_[i]);
            Pose pose_pi = Pose(pose_pivot.T_ .inverse() * pose_i.T_ * pose_ext.T_);

            pcl::transformPointCloud(surf_points_stack_[IDX_REF][i], surf_points_trans, pose_pi.T_.cast<float>());
            for (auto &p: surf_points_trans.points) p.intensity = i;
            surf_points_tmp += surf_points_trans;

            pcl::transformPointCloud(corner_points_stack_[IDX_REF][i], corner_points_trans, pose_pi.T_.cast<float>());
            for (auto &p: corner_points_trans.points) p.intensity = i;
            corner_points_tmp += corner_points_trans;
        }
        surf_points_stack_[IDX_REF][pivot_idx] = surf_points_tmp;
        corner_points_stack_[IDX_REF][pivot_idx] = corner_points_tmp;
        ini_fixed_local_map_ = true;
    }

    // -----------------
    // build the whole local map using all poses except the newest pose
    for (int n = 0; n < NUM_OF_LASER; n++)
    {
        surf_points_local_map_[n].clear();
        surf_points_local_map_filtered_[n].clear();
        corner_points_local_map_[n].clear();
        corner_points_local_map_filtered_[n].clear();
    }

    for (int n = 0; n < NUM_OF_LASER; n++)
    {
        Pose pose_ext = Pose(qbl_[n], tbl_[n]);
        for (int i = 0; i < WINDOW_SIZE + 1; i++)
        {
            Pose pose_i(Qs_[i], Ts_[i]);
            pose_local_[n][i] = Pose(pose_pivot.T_.inverse() * pose_i.T_ * pose_ext.T_);
            if ((i < pivot_idx) || (i == WINDOW_SIZE)) continue;
            if (n == IDX_REF) // localmap of reference
            {
                pcl::transformPointCloud(surf_points_stack_[n][i], surf_points_trans, pose_local_[n][i].T_.cast<float>());
                for (auto &p: surf_points_trans.points) p.intensity = i;
                surf_points_local_map_[n] += surf_points_trans;

                pcl::transformPointCloud(corner_points_stack_[n][i], corner_points_trans, pose_local_[n][i].T_.cast<float>());
                for (auto &p: corner_points_trans.points) p.intensity = i;
                corner_points_local_map_[n] += corner_points_trans;
            }
        }

        pcl::VoxelGrid<PointI> down_size_filter;
        if (n == IDX_REF)
        {
            down_size_filter.setLeafSize(0.4, 0.4, 0.4);
            down_size_filter.setInputCloud(boost::make_shared<PointICloud>(surf_points_local_map_[n]));
            down_size_filter.filter(surf_points_local_map_filtered_[n]);
            down_size_filter.setInputCloud(boost::make_shared<PointICloud>(corner_points_local_map_[n]));
            down_size_filter.filter(corner_points_local_map_filtered_[n]);
        } else
        {
            down_size_filter.setLeafSize(0.3, 0.3, 0.3);
            down_size_filter.setInputCloud(boost::make_shared<PointICloud>(surf_points_local_map_[IDX_REF]));
            down_size_filter.filter(surf_points_local_map_filtered_[n]); // filter surf_local_map
            down_size_filter.setInputCloud(boost::make_shared<PointICloud>(corner_points_local_map_[IDX_REF]));
            down_size_filter.filter(corner_points_local_map_filtered_[n]); // filter corner_local_map
        }
    }

    // -----------------
    // calculate features and correspondences from p+1 to j
    TicToc t_map_extract;
    surf_map_features_.clear();
    surf_map_features_.resize(NUM_OF_LASER);
    corner_map_features_.clear();
    corner_map_features_.resize(NUM_OF_LASER);
    for (int n = 0; n < NUM_OF_LASER; n++)
    {
        surf_map_features_[n].resize(WINDOW_SIZE + 1);
        pcl::KdTreeFLANN<PointI>::Ptr kdtree_surf_points_local_map(new pcl::KdTreeFLANN<PointI>());
        kdtree_surf_points_local_map->setInputCloud(boost::make_shared<PointICloud>(surf_points_local_map_filtered_[n]));

        corner_map_features_[n].resize(WINDOW_SIZE + 1);
        pcl::KdTreeFLANN<PointI>::Ptr kdtree_corner_points_local_map(new pcl::KdTreeFLANN<PointI>());
        kdtree_corner_points_local_map->setInputCloud(boost::make_shared<PointICloud>(corner_points_local_map_filtered_[n]));

        int n_neigh = (n == IDX_REF ? 5:10);
        // int n_neigh = 5;
        for (int i = pivot_idx; i < WINDOW_SIZE + 1; i++)
        {
            if (((n == IDX_REF) && (i == pivot_idx)) || ((n != IDX_REF) && (i != pivot_idx))) continue;
            f_extract_.extractSurfFromMap(kdtree_surf_points_local_map, surf_points_local_map_filtered_[n],
                surf_points_stack_[n][i], pose_local_[n][i], surf_map_features_[n][i], n_neigh);
            f_extract_.extractCornerFromMap(kdtree_corner_points_local_map, corner_points_local_map_filtered_[n],
                corner_points_stack_[n][i], pose_local_[n][i], corner_map_features_[n][i], n_neigh);
        }
    }
    printf("build map: %fms\n", t_build_map.toc());
    if (PCL_VIEWER) visualizePCL();
}

void Estimator::buildLocalMap()
{
    TicToc t_build_map;
    int pivot_idx = WINDOW_SIZE - OPT_WINDOW_SIZE;
    Pose pose_pivot(Qs_[pivot_idx], Ts_[pivot_idx]);

    // -----------------
    // build static local map using fixed poses
    PointICloud surf_points_trans, corner_points_trans;
    if (!ini_fixed_local_map_)
    {
        PointICloud surf_points_tmp, corner_points_tmp;
        for (int n = 0; n < NUM_OF_LASER; n++)
        {
            Pose pose_ext = Pose(qbl_[n], tbl_[n]);
            for (int i = 0; i <= pivot_idx; i++)
            {
                Pose pose_i(Qs_[i], Ts_[i]);
                Pose pose_ext_pi = Pose(pose_pivot.T_.inverse() * pose_i.T_ * pose_ext.T_);
                pcl::transformPointCloud(surf_points_stack_[n][i], surf_points_trans, pose_ext_pi.T_.cast<float>());
                for (auto &p: surf_points_trans.points) p.intensity = i;
                surf_points_tmp += surf_points_trans;
                // pcl::transformPointCloud(corner_points_stack_[n][i], corner_points_trans, pose_ext_pi.T_.cast<float>());
                // for (auto &p: corner_points_trans.points) p.intensity = i;
                // corner_points_tmp += corner_points_trans;
            }
            surf_points_stack_[n][pivot_idx] = surf_points_tmp;
            // corner_points_stack_[n][pivot_idx] = corner_points_tmp;
        }
        ini_fixed_local_map_ = true;
    }

    // -----------------
    // build the whole local map using all poses except the newest pose
    for (int n = 0; n < NUM_OF_LASER; n++)
    {
        surf_points_local_map_[n].clear();
        surf_points_local_map_filtered_[n].clear();
        // corner_points_local_map_[n].clear();
        // corner_points_local_map_filtered_[n].clear();
    }

    for (int n = 0; n < NUM_OF_LASER; n++)
    {
        Pose pose_ext = Pose(qbl_[n], tbl_[n]);
        for (int i = 0; i < WINDOW_SIZE + 1; i++)
        {
            Pose pose_i(Qs_[i], Ts_[i]);
            pose_local_[n][i] = Pose(pose_pivot.T_.inverse() * pose_i.T_ * pose_ext.T_);
            if ((i < pivot_idx) || (i == WINDOW_SIZE)) continue;
            pcl::transformPointCloud(surf_points_stack_[n][i], surf_points_trans, pose_local_[n][i].T_.cast<float>());
            for (auto &p: surf_points_trans.points) p.intensity = i;
            surf_points_local_map_[n] += surf_points_trans;
            // pcl::transformPointCloud(corner_points_stack_[n][i], corner_points_trans, pose_local_[n][i].T_.cast<float>());
            // for (auto &p: corner_points_trans.points) p.intensity = i;
            // corner_points_local_map_[n] += corner_points_trans;
        }
        pcl::VoxelGrid<PointI> down_size_filter;
        down_size_filter.setLeafSize(0.4, 0.4, 0.4);
        down_size_filter.setInputCloud(boost::make_shared<PointICloud>(surf_points_local_map_[n]));
        down_size_filter.filter(surf_points_local_map_filtered_[n]);
        // down_size_filter.setInputCloud(boost::make_shared<PointICloud>(corner_points_local_map_[n]));
        // down_size_filter.filter(corner_points_local_map_filtered_[n]);
    }

    // -----------------
    // calculate features and correspondences from p+1 to j
    surf_map_features_.clear();
    surf_map_features_.resize(NUM_OF_LASER);
    for (int n = 0; n < NUM_OF_LASER; n++)
    {
        surf_map_features_[n].resize(WINDOW_SIZE + 1);
        pcl::KdTreeFLANN<PointI>::Ptr kdtree_surf_points_local_map(new pcl::KdTreeFLANN<PointI>());
        kdtree_surf_points_local_map->setInputCloud(boost::make_shared<PointICloud>(surf_points_local_map_filtered_[n]));
        // corner_map_features_[n].resize(WINDOW_SIZE + 1);
        // pcl::KdTreeFLANN<PointI>::Ptr kdtree_corner_points_local_map(new pcl::KdTreeFLANN<PointI>());
        // kdtree_corner_points_local_map->setInputCloud(boost::make_shared<PointICloud>(corner_points_local_map_filtered_[n]));
        int n_neigh = 5;
        for (int i = pivot_idx + 1; i < WINDOW_SIZE + 1; i++)
        {
            f_extract_.extractSurfFromMap(kdtree_surf_points_local_map, surf_points_local_map_filtered_[n],
                surf_points_stack_[n][i], pose_local_[n][i], surf_map_features_[n][i], n_neigh);
            // f_extract_.extractCornerFromMap(kdtree_corner_points_local_map, corner_points_local_map_filtered_[n],
            //     corner_points_stack_[n][i], pose_local_[n][i], corner_map_features_[n][i], n_neigh);
        }
    }
    printf("build map: %fms\n", t_build_map.toc());

    if (PCL_VIEWER) visualizePCL();
}

// push new state and measurements in the sliding window
// move the localmap in the pivot frame to the pivot+1 frame, and remove the first point cloud
void Estimator::slideWindow()
{
    TicToc t_solid_window;
    printf("sliding window with cir_buf_cnt_: %d\n", cir_buf_cnt_);
    if (ini_fixed_local_map_)
    {
        int pivot_idx = WINDOW_SIZE - OPT_WINDOW_SIZE;
        Pose pose_pivot(Qs_[pivot_idx], Ts_[pivot_idx]);

        int i = pivot_idx + 1;
        Pose pose_i(Qs_[i], Ts_[i]);
        for (int n = 0; n < NUM_OF_LASER; n++)
        {
            if ((ESTIMATE_EXTRINSIC == 1) && (n != IDX_REF)) continue;

            PointICloud surf_points_trans, surf_points_filtered, corner_points_trans, corner_points_filtered;
            Pose pose_ext = Pose(qbl_[n], tbl_[n]);
            Pose pose_i_pivot = Pose((pose_i.T_ * pose_ext.T_).inverse() * pose_pivot.T_ * pose_ext.T_);
            pcl::ExtractIndices<PointI> extract;

            pcl::transformPointCloud(surf_points_stack_[n][pivot_idx], surf_points_trans, pose_i_pivot.T_.cast<float>());
            pcl::PointIndices::Ptr inliers_surf(new pcl::PointIndices());
            for (int j = 0; j < surf_points_stack_size_[n][0]; j++) inliers_surf->indices.push_back(j);
            extract.setInputCloud(boost::make_shared<PointICloud>(surf_points_trans));
            extract.setIndices(inliers_surf);
            extract.setNegative(true);
            extract.filter(surf_points_filtered);
            surf_points_filtered += surf_points_stack_[n][i];
            surf_points_stack_[n][i] = surf_points_filtered;

            if (ESTIMATE_EXTRINSIC == 0) continue;
            pcl::transformPointCloud(corner_points_stack_[n][pivot_idx], corner_points_trans, pose_i_pivot.T_.cast<float>());
            pcl::PointIndices::Ptr inliers_corner(new pcl::PointIndices());
            for (int j = 0; j < corner_points_stack_size_[n][0]; j++) inliers_corner->indices.push_back(j);
            extract.setInputCloud(boost::make_shared<PointICloud>(corner_points_trans));
            extract.setIndices(inliers_corner);
            extract.setNegative(true);
            extract.filter(corner_points_filtered);
            corner_points_filtered += corner_points_stack_[n][i];
            corner_points_stack_[n][i] = corner_points_filtered;
        }
    }

    Qs_.push(Qs_[cir_buf_cnt_]);
    Ts_.push(Ts_[cir_buf_cnt_]);
    Header_.push(Header_[cir_buf_cnt_]);
    for (int n = 0; n < NUM_OF_LASER; n++)
    {
        surf_points_stack_[n].push(surf_points_stack_[n][cir_buf_cnt_]);
        surf_points_stack_size_[n].push(surf_points_stack_size_[n][cir_buf_cnt_]);
        corner_points_stack_[n].push(corner_points_stack_[n][cir_buf_cnt_]);
        corner_points_stack_size_[n].push(corner_points_stack_size_[n][cir_buf_cnt_]);
    }
    // printf("slide window: %fms\n", t_solid_window.toc());
}

void Estimator::vector2Double()
{
    int pivot_idx = WINDOW_SIZE - OPT_WINDOW_SIZE;
    for (int i = pivot_idx; i < WINDOW_SIZE + 1; i++)
    {
        para_pose_[i - pivot_idx][0] = Ts_[i](0);
        para_pose_[i - pivot_idx][1] = Ts_[i](1);
        para_pose_[i - pivot_idx][2] = Ts_[i](2);
        para_pose_[i - pivot_idx][3] = Qs_[i].x();
        para_pose_[i - pivot_idx][4] = Qs_[i].y();
        para_pose_[i - pivot_idx][5] = Qs_[i].z();
        para_pose_[i - pivot_idx][6] = Qs_[i].w();
    }
    for (int i = 0; i < NUM_OF_LASER; i++)
    {
        para_ex_pose_[i][0] = tbl_[i](0);
        para_ex_pose_[i][1] = tbl_[i](1);
        para_ex_pose_[i][2] = tbl_[i](2);
        para_ex_pose_[i][3] = qbl_[i].x();
        para_ex_pose_[i][4] = qbl_[i].y();
        para_ex_pose_[i][5] = qbl_[i].z();
        para_ex_pose_[i][6] = qbl_[i].w();
    }
    // for (int i = 0; i < NUM_OF_LASER; i++)
    // {
    //     para_td_[i] = tdbl_[i];
    // }
}

void Estimator::double2Vector()
{
    int pivot_idx = WINDOW_SIZE - OPT_WINDOW_SIZE;
    for (int i = 0; i < OPT_WINDOW_SIZE + 1; i++)
    {
        Ts_[i + pivot_idx] = Eigen::Vector3d(para_pose_[i][0], para_pose_[i][1], para_pose_[i][2]);
        Qs_[i + pivot_idx] = Eigen::Quaterniond(para_pose_[i][6], para_pose_[i][3], para_pose_[i][4], para_pose_[i][5]);
    }
    for (int i = 0; i < NUM_OF_LASER; i++)
    {
        tbl_[i] = Eigen::Vector3d(para_ex_pose_[i][0], para_ex_pose_[i][1], para_ex_pose_[i][2]);
        qbl_[i] = Eigen::Quaterniond(para_ex_pose_[i][6], para_ex_pose_[i][3], para_ex_pose_[i][4], para_ex_pose_[i][5]);
    }
    // for (int i = 0; i < NUM_OF_LASER; i++)
    // {
    //     tdbl_[i] = para_td_[i];
    // }
}

void Estimator::evalResidual(ceres::Problem &problem,
    std::vector<PoseLocalParameterization *> &local_param_ids,
    const std::vector<double *> &para_ids,
    const std::vector<ceres::internal::ResidualBlock *> &res_ids_proj,
	const MarginalizationInfo *last_marginalization_info_,
    const std::vector<ceres::internal::ResidualBlock *> &res_ids_marg,
    const bool b_eval_degenracy)
{
	double cost;
    ceres::CRSMatrix jaco;
    ceres::Problem::EvaluateOptions e_option;
	if ((PRIOR_FACTOR) || (POINT_PLANE_FACTOR) || (POINT_EDGE_FACTOR))
	{
		e_option.parameter_blocks = para_ids;
		e_option.residual_blocks = res_ids_proj;
		problem.Evaluate(e_option, &cost, NULL, NULL, &jaco);
        printf("cost proj: %f\n", cost);
        if (b_eval_degenracy) evalDegenracy(local_param_ids, jaco);
	}
	if (MARGINALIZATION_FACTOR)
	{
		if (last_marginalization_info_ && !res_ids_marg.empty())
		{
			e_option.parameter_blocks = para_ids;
			e_option.residual_blocks = res_ids_marg;
			problem.Evaluate(e_option, &cost, NULL, NULL, &jaco);
            printf("cost marg: %f\n", cost);
		}
	}
}

// A^TA is not only symmetric and invertiable: https://math.stackexchange.com/questions/2352684/when-is-a-symmetric-matrix-invertible
void Estimator::evalDegenracy(std::vector<PoseLocalParameterization *> &local_param_ids, const ceres::CRSMatrix &jaco)
{
    printf("jacob: %d constraints, %d parameters (%d pose_param, %d ext_param)\n", jaco.num_rows, jaco.num_cols, 6*(OPT_WINDOW_SIZE + 1), 6*NUM_OF_LASER); // 58, feature_size(2700) x 50
    TicToc t_eval_degenracy;
    Eigen::MatrixXd mat_J_raw;
    CRSMatrix2EigenMatrix(jaco, mat_J_raw);
    {
        Eigen::MatrixXd &mat_J = mat_J_raw;
        Eigen::MatrixXd mat_Jt = mat_J.transpose(); // A^T
        Eigen::MatrixXd mat_JtJ = mat_Jt * mat_J; // A^TA 48*48
        bool b_vis = false; // to verify the structure of A^T*A
        if (b_vis)
        {
            printf("visualize the structure of H(J^T*J)\n");
            for (int i = 0; i < mat_JtJ.rows(); i++)
            {
                for (int j = 0; j < mat_JtJ.cols(); j++)
                {
                    if (mat_JtJ(i, j) == 0) std::cout << "0 ";
                                        else std::cout << "1 ";
                }
                std::cout << std::endl;
            }
        }

        double eig_thre; // the larger, the better (with more constraints)
        for (int i = 0; i < local_param_ids.size(); i++)
        {
            Eigen::Matrix<double, 6, 6> mat_H = mat_JtJ.block(6*i, 6*i, 6, 6);
            local_param_ids[i]->setParameter();
            Eigen::SelfAdjointEigenSolver<Eigen::Matrix<double, 6, 6> > esolver(mat_H);
            Eigen::Matrix<double, 1, 6> mat_E = esolver.eigenvalues().real(); // 6*1
            Eigen::Matrix<double, 6, 6> mat_V_f = esolver.eigenvectors().real(); // 6*6, column is the corresponding eigenvector
            Eigen::Matrix<double, 6, 6> mat_V_p = mat_V_f;

            local_param_ids[i]->is_degenerate_ = false;
            eig_thre = eig_thre_calib_[i];
            for (int j = 0; j < mat_E.cols(); j++)
            {
                if (mat_E(0, j) < eig_thre)
                {
                    mat_V_p.col(j) = Eigen::Matrix<double, 6, 1>::Zero();
                    local_param_ids[i]->is_degenerate_ = true;
                } else
                {
                    break;
                }
            }
            std::cout << i << ": D factor: " << mat_E(0, 0) << ", D vector: " << mat_V_f.col(0).transpose() << std::endl;
            Eigen::Matrix<double, 6, 6> mat_P = (mat_V_f.transpose()).inverse() * mat_V_p.transpose(); // 6*6
            assert(mat_P.rows() == 6);

            if (i > OPT_WINDOW_SIZE)
            {
                if (mat_E(0, 0) > eig_thre)
                    eig_thre_calib_[i] = mat_E(0, 0);
                else
                    mat_P.setZero();
            }
            if (local_param_ids[i]->is_degenerate_)
            {
                local_param_ids[i]->V_update_ = mat_P;
                // std::cout << "param " << i << " is degenerate !" << std::endl;
                // std::cout << mat_P << std::endl;
            }
        }
        std::cout << "eigen threshold " << eig_thre_calib_.size() << ": ";
        for (int i = 0; i < eig_thre_calib_.size(); i++) std::cout << eig_thre_calib_[i] << " ";
        std::cout << std::endl;
    }
    printf("evaluate degeneracy %fms\n", t_eval_degenracy.toc());
}

void Estimator::evalCalib()
{
    if (solver_flag_ == NON_LINEAR)
    {
        bool is_converage = true;
        // TODO: very rough calibration stability analysis
        for (int i = 0; i < NUM_OF_LASER; i++)
            if ((i != IDX_REF) && (eig_thre_calib_[i + OPT_WINDOW_SIZE + 1] < EIG_THRE_CALIB))
            {
                is_converage = false;
                break;
            }
        if (is_converage)
        {
            ROS_WARN("Finish nonlinear calibration !");
            ESTIMATE_EXTRINSIC = 0;
            ini_fixed_local_map_ = false; // reconstruct new optimized map
            if (last_marginalization_info_ != nullptr) delete last_marginalization_info_;
            last_marginalization_info_ = nullptr; // meaning that the prior errors in online calibration are discarded
            last_marginalization_parameter_blocks_.clear();
        }
    }
}

void Estimator::visualizePCL()
{
    if (plane_normal_vis_.init_)
    {
        PointCloud::Ptr point_world_xyz(new PointCloud);
        pcl::copyPointCloud(surf_points_local_map_filtered_[1], *point_world_xyz);
        plane_normal_vis_.UpdateCloud(point_world_xyz, "cloud_all");
    }
    int pivot_idx = WINDOW_SIZE - OPT_WINDOW_SIZE;
    std::vector<Eigen::Vector4d, Eigen::aligned_allocator<Eigen::Vector4d> > plane_coeffs;
    PointCloud::Ptr tmp_cloud_sel(new PointCloud); // surf_points_stack_[n][i]
    NormalCloud::Ptr tmp_normals_sel(new NormalCloud); // surf_points_local_map_filtered_[n]
    printf("feature size: %u\n", surf_map_features_[1][WINDOW_SIZE].size());
    for (auto &f : surf_map_features_[1][WINDOW_SIZE])
    {
        PointI p_ori;
        p_ori.x = f.point_.x();
        p_ori.y = f.point_.y();
        p_ori.z = f.point_.z();
        PointI p_sel;
        f_extract_.pointAssociateToMap(p_ori, p_sel, pose_local_[1][WINDOW_SIZE]);
        tmp_cloud_sel->push_back(Point{p_sel.x, p_sel.y, p_sel.z}); // target cloud
        tmp_normals_sel->push_back(Normal{float(f.coeffs_.x()), float(f.coeffs_.y()), float(f.coeffs_.z())}); // reference cloud normal
        // Eigen::Vector4d coeffs_normalized = f.coeffs;
        // double s_normal = coeffs_normalized.head<3>().norm();
        // coeffs_normalized = coeffs_normalized / s_normal;
        // plane_coeffs.push_back(coeffs_normalized);
        // DLOG(INFO) << p_sel.x * f.coeffs.x() + p_sel.y * f.coeffs.y() + p_sel.z * f.coeffs.z() + f.coeffs.w();
    }
    if (plane_normal_vis_.init_)
    {
        plane_normal_vis_.UpdateCloudAndNormals(tmp_cloud_sel, tmp_normals_sel, PCL_VIEWER_NORMAL_RATIO, "cloud1", "normal1");
    }
    // std::cout << "pose pivot to j: " << pose_local_[1][WINDOW_SIZE] << std::endl;
}

void Estimator::printParameter()
{
    printf("print optimized window (p -> j) [qx qy qz qw x y z]\n");
    for (int i = 0; i < OPT_WINDOW_SIZE + 1; i++)
    {
        std::cout << "Pose " << WINDOW_SIZE - OPT_WINDOW_SIZE + i << ": " <<
            para_pose_[i][3] << " " <<
            para_pose_[i][4] << " " <<
            para_pose_[i][5] << " " <<
            para_pose_[i][6] << " " <<
            para_pose_[i][0] << " " <<
            para_pose_[i][1] << " " <<
            para_pose_[i][2] << std::endl;
    }
    for (int i = 0; i < NUM_OF_LASER; i++)
    {
        std::cout << "Ext: " << " " <<
            para_ex_pose_[i][3] << " " <<
            para_ex_pose_[i][4] << " " <<
            para_ex_pose_[i][5] << " " <<
            para_ex_pose_[i][6] << " " <<
            para_ex_pose_[i][0] << " " <<
            para_ex_pose_[i][1] << " " <<
            para_ex_pose_[i][2] << std::endl;
    }
    // for (int i = 0; i < NUM_OF_LASER; i++)
    // {
    //     std::cout << "dt: " <<
    //         para_td_[i] << std::endl;
    // }
}

void Estimator::printSlideWindow()
{
    printf("print slide window (0 -> j) ************************\n");
    for (int i = 0; i < cir_buf_cnt_ + 1; i++)
    {
        Pose pose(Qs_[i], Ts_[i]);
        std::cout << i << ": " << pose << std::endl;
    }
}

//
