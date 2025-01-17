#include <stdio.h>
#include <queue>
#include <map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <ros/ros.h>
#include <cv_bridge/cv_bridge.h>
#include <opencv2/opencv.hpp>

#include "estimator.h"
#include "parameters.h"
#include "utility/visualization.h"

 
Esimator estimator;  //构造函数初始化一些参数为  单位矩阵，零矩阵，或者零

std::condition_variable con;
double current_time = -1;
queue<sensor_msgs::ImuConstPtr> imu_buf;
queue<sensor_msgs::PointCloudConstPtr> feature_buf;
queue<sensor_msgs::PointCloudConstPtr> relo_buf;
int sum_of_wait = 0;

std::mutex m_buf;
std::mutex m_state;
std::mutex i_buf;
std::mutex m_estimator;

double latest_time;
Eigen::Vector3d tmp_P;
Eigen::Quaterniond tmp_Q;
Eigen::Vector3d tmp_V;
Eigen::Vector3d tmp_Ba;
Eigen::Vector3d tmp_Bg;
Eigen::Vector3d acc_0;
Eigen::Vector3d gyr_0;
bool init_feature = 0;
bool init_imu = 1;
double last_imu_t = 0;

/*
//vio初始时刻建立的坐标系暂时称为世界坐标系w
//imu自己的坐标系称为body坐标系b
//predict函数计算了dt时间段的世界坐标系w下的平均加速度（通过前后两次的w坐标系下的加速度的平均值得到），加计平均值un_acc采用的是减去偏置和估计的重力之后的加计值
//predict函数计算了b系相对于w系的旋转四元数tmp_Q，通过上次tmp_Q乘以本次增量四元数得到，增量四元数通过角速度平均值乘以时间dt得到（因为角度小的时候，四元数可以近似用旋转角计算，进而近似用欧拉角计算）
//prddict函数最终计算目标量：tmp_V速度，tmp_P位置，tmp_Q四元数都是相对于w系的*/
*/
void predict(const sensor_msgs::ImuConstPtr &imu_msg)
{
    double t = imu_msg->header.stamp.toSec();
    if (init_imu)
    {
        latest_time = t;
        init_imu = 0;
        return;
    }
    double dt = t - latest_time;
    latest_time = t;

    double dx = imu_msg->linear_acceleration.x;
    double dy = imu_msg->linear_acceleration.y;
    double dz = imu_msg->linear_acceleration.z;
    
    Eigen::Vector3d linear_acceleration{dx, dy, dz};

    double rx = imu_msg->angular_velocity.x;
    double ry = imu_msg->angular_velocity.y;
    double rz = imu_msg->angular_velocity.z;
    Eigen::Vector3d angular_velocity{rx, ry, rz};

    Eigen::Vector3d un_acc_0 = tmp_Q * (acc_0 - tmp_Ba) - estimator.g;

    Eigen::Vector3d un_gyr = 0.5 * (gyr_0 + angular_velocity) - tmp_Bg;  // 平均角速度
    tmp_Q = tmp_Q * Utility::deltaQ(un_gyr * dt);                        // 四元数更新 乘法

    Eigen::Vector3d un_acc_1 = tmp_Q * (linear_acceleration - tmp_Ba) - estimator.g;

    Eigen::Vector3d un_acc = 0.5 * (un_acc_0 + un_acc_1);           // 前后两帧的加速度平均（世界坐标系且减去重力）

    tmp_P = tmp_P + dt * tmp_V + 0.5 * dt * dt * un_acc;  // s = s + vt + 1/2a*t^2
    tmp_V = tmp_V + dt * un_acc;                          // v = v + a*t

    acc_0 = linear_acceleration;
    gyr_0 = angular_velocity;
}

void update()
{
    TicToc t_predict;
    latest_time = current_time;
    tmp_P = estimator.Ps[WINDOW_SIZE];
    tmp_Q = estimator.Rs[WINDOW_SIZE];
    tmp_V = estimator.Vs[WINDOW_SIZE];
    tmp_Ba = estimator.Bas[WINDOW_SIZE];
    tmp_Bg = estimator.Bgs[WINDOW_SIZE];
    acc_0 = estimator.acc_0;
    gyr_0 = estimator.gyr_0;

    queue<sensor_msgs::ImuConstPtr> tmp_imu_buf = imu_buf;
    for (sensor_msgs::ImuConstPtr tmp_imu_msg; !tmp_imu_buf.empty(); tmp_imu_buf.pop())
        predict(tmp_imu_buf.front());

}


/*
如果数据满足要求，返回某一帧的图像信息以及该帧和上一帧之间的所有imu信息：imu和图像组成pair。
*/
std::vector<std::pair<std::vector<sensor_msgs::ImuConstPtr>, sensor_msgs::PointCloudConstPtr>>
getMeasurements()
{
    std::vector<std::pair<std::vector<sensor_msgs::ImuConstPtr>, sensor_msgs::PointCloudConstPtr>> measurements;

    while (true)
    {
        if (imu_buf.empty() || feature_buf.empty())
            return measurements;

        /* imu的最后时间戳 还小于图像时间 ，则需要等待IMU */

        if (!(imu_buf.back()->header.stamp.toSec() > feature_buf.front()->header.stamp.toSec() + estimator.td))  //注意这里的imu_buf.back() 和 imu_buf.front()
        {
            //ROS_WARN("wait for imu, only should happen at the beginning");，因为后面的情况下，IMu时间会在图像时间之后
            sum_of_wait++;
            return measurements;
        }

        /* imu的最前时间戳已经大于图像时间，则需要扔掉 图像*/
        if (!(imu_buf.front()->header.stamp.toSec() < feature_buf.front()->header.stamp.toSec() + estimator.td))  // 这里的时间戳比较是整个buf的时间戳比较
        {
            ROS_WARN("throw img, only should happen at the beginning");
            feature_buf.pop();
            continue;
        }
        // 上述操作保证 了IMu和图像的时间戳同步

        sensor_msgs::PointCloudConstPtr img_msg = feature_buf.front();  // 取  一帧img
        feature_buf.pop();


        std::vector<sensor_msgs::ImuConstPtr> IMUs;
        while (imu_buf.front()->header.stamp.toSec() < img_msg->header.stamp.toSec() + estimator.td)   // 这里是单个图像估计的时间比较
        {
            IMUs.emplace_back(imu_buf.front());
            imu_buf.pop();
        }

        /*
        多取了时间戳大于(取到后从imu_buf中丢弃)或者等于图像时间戳的一帧imu
        (该帧取后并没有从imu_buf中被丢弃，取下帧图像时可作为头帧被取到)

        为了可以在process函数里将加计和角计  插值得到img时间戳时刻的acc，gyro近似值  这个是个小trick
        */

        IMUs.emplace_back(imu_buf.front()); // 多取了时间戳大于(取到后从imu_buf中丢弃)或者等于图像时间戳的一帧imu 而且 这里没有丢弃imu
        if (IMUs.empty())
            ROS_WARN("no imu between two image");

        measurements.emplace_back(IMUs, img_msg);  // 一帧图对应了很多的 IMU 
    }
    return measurements;
}

/*核心函數，输入IMU数据
输出 Pos、Q、vel
*/
void imu_callback(const sensor_msgs::ImuConstPtr &imu_msg)
{
    if (imu_msg->header.stamp.toSec() <= last_imu_t) // 时间戳不对
    {
        ROS_WARN("imu message in disorder!");
        return;
    }
    last_imu_t = imu_msg->header.stamp.toSec();

    m_buf.lock();
    imu_buf.push(imu_msg);   // imu_msg 原始数据放入  imu_buf
    m_buf.unlock();
    con.notify_one();

    last_imu_t = imu_msg->header.stamp.toSec();

    {
        std::lock_guard<std::mutex> lg(m_state);
        predict(imu_msg);   // 计算IMU原始数据

        std_msgs::Header header = imu_msg->header;
        header.frame_id = "world";
        if (estimator.solver_flag == Estimator::SolverFlag::NON_LINEAR)
            pubLatestOdometry(tmp_P, tmp_Q, tmp_V, header);  
            
            //tmp_P, tmp_Q, tmp_V通过函数 predict(imu_msg)计算，然后发布到"imu_propagate"topic上
    }
}


void feature_callback(const sensor_msgs::PointCloudConstPtr &feature_msg)
{
    if (!init_feature)
    {
        //skip the first detected feature, which doesn't contain optical flow speed
        init_feature = 1;
        return;
    }
    m_buf.lock();
    feature_buf.push(feature_msg);
    m_buf.unlock();
    con.notify_one();
}

void restart_callback(const std_msgs::BoolConstPtr &restart_msg)
{
    if (restart_msg->data == true)
    {
        ROS_WARN("restart the estimator!");
        m_buf.lock();
        while(!feature_buf.empty())
            feature_buf.pop();
        while(!imu_buf.empty())
            imu_buf.pop();


        m_buf.unlock();


        m_estimator.lock();
        estimator.clearState();
        estimator.setParameter();
        m_estimator.unlock();


        current_time = -1;
        last_imu_t = 0;
    }
    return;
}

void relocalization_callback(const sensor_msgs::PointCloudConstPtr &points_msg)
{
    //printf("relocalization callback! \n");
    m_buf.lock();
    relo_buf.push(points_msg);
    m_buf.unlock();
}

// thread: visual-inertial odometry

/*
    这个主函数有点复杂呀
*/
void process()          //处理IMU和图像数据
{
    while (true)
    {
        std::vector<std::pair<std::vector<sensor_msgs::ImuConstPtr>, sensor_msgs::PointCloudConstPtr>> measurements;
        std::unique_lock<std::mutex> lk(m_buf);
        con.wait(lk, [&]
                 {
            return (measurements = getMeasurements()).size() != 0;
                 });
        lk.unlock();
        m_estimator.lock();  // 上述获取一帧图像会获取一组IMu数据，且时间上对齐了的

        for (auto &measurement : measurements)  // 测量值有两部分，一个是IMUbuf，一个是点云指针
        {
            auto img_msg = measurement.second;   

            double dx = 0, dy = 0, dz = 0, rx = 0, ry = 0, rz = 0;
            for (auto &imu_msg : measurement.first)
            {
                double t = imu_msg->header.stamp.toSec();
                double img_t = img_msg->header.stamp.toSec() + estimator.td; //时间戳小于本帧图像的imu信息，else中通过插值，计算图像时间戳时刻的加计和角计值。                                                                       
                if (t <= img_t)                                              //之后将加计和角计值都送入imu处理的主要函数：
                { 
                    if (current_time < 0)
                        current_time = t;
                    double dt = t - current_time;
                    ROS_ASSERT(dt >= 0);
                    current_time = t;
                    dx = imu_msg->linear_acceleration.x;
                    dy = imu_msg->linear_acceleration.y;
                    dz = imu_msg->linear_acceleration.z;
                    rx = imu_msg->angular_velocity.x;
                    ry = imu_msg->angular_velocity.y;
                    rz = imu_msg->angular_velocity.z;

                    estimator.processIMU(dt, Vector3d(dx, dy, dz), Vector3d(rx, ry, rz));
                    //printf("imu: dt:%f a: %f %f %f w: %f %f %f\n",dt, dx, dy, dz, rx, ry, rz);

                }
                else   // 根据IMU的时间戳 做一个插值
                {
                    double dt_1 = img_t - current_time;  
                    double dt_2 = t - img_t;  

                    current_time = img_t;
                    ROS_ASSERT(dt_1 >= 0);
                    ROS_ASSERT(dt_2 >= 0);
                    ROS_ASSERT(dt_1 + dt_2 > 0);

                    double w1 = dt_2 / (dt_1 + dt_2);
                    double w2 = dt_1 / (dt_1 + dt_2);

                    dx = w1 * dx + w2 * imu_msg->linear_acceleration.x;   //时间戳做权重
                    dy = w1 * dy + w2 * imu_msg->linear_acceleration.y;
                    dz = w1 * dz + w2 * imu_msg->linear_acceleration.z;
                    rx = w1 * rx + w2 * imu_msg->angular_velocity.x;
                    ry = w1 * ry + w2 * imu_msg->angular_velocity.y;
                    rz = w1 * rz + w2 * imu_msg->angular_velocity.z;

                    estimator.processIMU(dt_1, Vector3d(dx, dy, dz), Vector3d(rx, ry, rz));  // 将加计和角计值都送入imu处理的主要函数
                    //printf("dimu: dt:%f a: %f %f %f w: %f %f %f\n",dt_1, dx, dy, dz, rx, ry, rz);
                }
            }  //   这样基本保证 IMu数据和图片数据能对齐

            // set relocalization frame
            sensor_msgs::PointCloudConstPtr relo_msg = NULL;

            while (!relo_buf.empty())
            {
                relo_msg = relo_buf.front();
                relo_buf.pop();
            }

            /*
                //每个relo_msg首先包含的是一幅图像的所有特征点，所谓的match_points，其实是特征点relo_msg->points[0-size].x y z
                //每个relo_msg还包含了本帧图像相对于w系的？平移:relo_msg->channels[0].values[0-2];旋转:relo_msg->channels[0].values[3-6];
                //每个relo_msg包含了本帧图像的id:relo_msg->channels[0].values[7]
            */

            if (relo_msg != NULL)
            {
                vector<Vector3d> match_points;
                double frame_stamp = relo_msg->header.stamp.toSec();

                for (unsigned int i = 0; i < relo_msg->points.size(); i++)  // 特征点
                {
                    Vector3d u_v_id;
                    u_v_id.x() = relo_msg->points[i].x;
                    u_v_id.y() = relo_msg->points[i].y;
                    u_v_id.z() = relo_msg->points[i].z;
                    match_points.push_back(u_v_id);
                }
                Vector3d relo_t(relo_msg->channels[0].values[0], relo_msg->channels[0].values[1], relo_msg->channels[0].values[2]);
                Quaterniond relo_q(relo_msg->channels[0].values[3], relo_msg->channels[0].values[4], relo_msg->channels[0].values[5], relo_msg->channels[0].values[6]);
                
                Matrix3d relo_r = relo_q.toRotationMatrix();

                int frame_index;
                frame_index = relo_msg->channels[0].values[7];

                //调用setReloFrame函数,虽然在measure的for循环中，但是读取的是从/pose_graph/match_points  订阅的图像特征以及旋转平移信息，每次都读最近的一条消息。
                estimator.setReloFrame(frame_stamp, frame_index, match_points, relo_t, relo_r);
                //将特征点，旋转平移放入  estimator中(match_points、prev_relo_t、prev_relo_r)，并找出该帧 relo_msg 和图像对应的时间戳，得到该帧的 relo_Pose
            }

            ROS_DEBUG("processing vision data with stamp %f \n", img_msg->header.stamp.toSec());

            TicToc t_s;

            map<int, vector<pair<int, Eigen::Matrix<double, 7, 1>>>> image;

            for (unsigned int i = 0; i < img_msg->points.size(); i++)  //对于每一帧图像的每个特征都有特定的 feature_id 
            {
                int v = img_msg->channels[0].values[i] + 0.5;
                
                int feature_id = v / NUM_OF_CAM;  //==>  1/1 = 1 ,2/1 = 2 100/1  ==100

                int camera_id = v % NUM_OF_CAM;  // 求余数，那会始终是  camera_id  0

                double x = img_msg->points[i].x;   
                double y = img_msg->points[i].y;
                double z = img_msg->points[i].z;

                double p_u = img_msg->channels[1].values[i];   
                double p_v = img_msg->channels[2].values[i];

                double velocity_x = img_msg->channels[3].values[i];
                double velocity_y = img_msg->channels[4].values[i];

                ROS_ASSERT(z == 1);
                Eigen::Matrix<double, 7, 1> xyz_uv_velocity;
                xyz_uv_velocity << x, y, z, p_u, p_v, velocity_x, velocity_y;
 
				image[feature_id].emplace_back(camera_id,  xyz_uv_velocity);   // 不同的特征有不同的 feature_id //image是个map,这个map包含等于相机数目  的图像上 所有的特征信息
            }// 数据预处理，放进image map中

            
            /*
            //image是个map,这个map包含  等于相机数目  的图像上 所有的特征信息
            //map image的first是不同的  feature_id  second是该特征id对应的   相机id和具体信息(特征点投影射线(已校正) 原像素位置(未校正)，速度(已校正算出的))
            
            //本来second应该是一个由<camera_id,  xyz_uv_velocity> 构成的pair来构成的向量
            实际上一幅图像img_msg的一种特征应该只对应一个pair 因此这个vector应该只有一个元素
            //如果是两个相机，一次传过由两幅图像的特征构成的一个img_msg
            两个图像上的相同的feature id也被编码为和相机号码有关的不同的值，都统一放在一个向量points中

            //然后在上面的for循环中被解算成相同的 feature_id
            进而根据camera_id的不同，构成的pair不同 而放在的同一个feature id对应的向量里 所以上面的pair写为了向量
            */

           
            estimator.processImage(image, img_msg->header); // 处理图像函数


            /*
            Vins_estimator_node里面用到了两次ceres优化，
            一次为初始化函数中的sfm.construct，利用重投影残差最小，优化了窗口里的所有平移旋转和三维坐标；
            另一次是在solveOdometry()中调用了optimization()，基本上优化了所有设计的参量。
            */

            double whole_t = t_s.toc();
            printStatistics(estimator, whole_t);

            std_msgs::Header header = img_msg->header;
            header.frame_id = "world";

            pubOdometry(estimator, header);
            pubKeyPoses(estimator, header);
            pubCameraPose(estimator, header);
            pubPointCloud(estimator, header);
            pubTF(estimator, header);
            pubKeyframe(estimator);

            if (relo_msg != NULL)
                pubRelocalization(estimator);
            //ROS_ERROR("end: %f, at %f", img_msg->header.stamp.toSec(), ros::Time::now().toSec());
        }

        m_estimator.unlock();
        m_buf.lock();
        m_state.lock();
        if (estimator.solver_flag == Estimator::SolverFlag::NON_LINEAR)
            update();
        m_state.unlock();
        m_buf.unlock();
    }
}


/*

（1）readParameters(n);//读取config文件夹下的相关yml文件
（2）estimator.setParameter();  //主要设置estimate中的外参旋转ric和平移tic的初值，以及feature中的ric初值，外参来自config文件
（3）registerPub(n);        //ros advertise初始化，发布消息前的工作

（4）ros::Subscriber sub_imu = n.subscribe(IMU_TOPIC, 2000, imu_callback,ros::TransportHints().tcpNoDelay());//transport_hints，允许你指定hints到roscpp的传输层，如更喜欢使用UPD传输，使用没延迟的TCP等
    其中（4）中调用了imu_callback回调函数，该回调函数做了一些工作：

    ①imu_buf.push(imu_msg); //imu_msg原始数据放入imu_buf
    ②predict(imu_msg)
        //vio初始时刻建立的坐标系暂时称为世界坐标系w
        //imu自己的坐标系称为body坐标系b
        //predict函数计算了dt时间段的世界坐标系w下的平均加速度（通过前后两次的w坐标系下的加速度的平均值得到），加计平均值un_acc采用的是减去偏置和估计的重力之后的加计值
        //predict函数计算了b系相对于w系的旋转四元数tmp_Q，通过上次tmp_Q乘以本次增量四元数得到，增量四元数通过角速度平均值乘以时间dt得到（因为角度小的时候，四元数可以近似用旋转角计算，进而近似用欧拉角计算）
        //prddict函数最终计算目标量：tmp_V速度，tmp_P位置，tmp_Q 四元数都是相对于w系的
    ③pubLatestOdometry(tmp_P, tmp_Q, tmp_V, header);//tmp_P, tmp_Q, tmp_V通过函数predict(imu_msg)计算，然后发布到"imu_propagate"topic上

    接着回到main函数：
（5）ros::Subscriber sub_image = n.subscribe("/feature_tracker/feature", 2000, feature_callback);
（6）ros::Subscriber sub_restart = n.subscribe("/feature_tracker/restart", 2000, restart_callback);
（7）ros::Subscriber sub_relo_points = n.subscribe("/pose_graph/match_points", 2000, relocalization_callback);
    这三个订阅的回调函数其中(5)(7)把数据存在相关的buf里：feature_buf, relo_buf
    (6)如果收到是restart 为true，则调用estimate的clearState()，然后再重新setParameter()设置外参，然后重新开始。


最后是正式开始处理的函数，都放在Process这个大函数中了，整个Process函数是一个线程，没有再多起线程（边缘化的时候起了4个线程计算jacobi，
但是计算完就结束了，不是一直执行，这个后面再详细说）
（8）std::thread measurement_process{process};

*/

int main(int argc, char **argv)
{
    ros::init(argc, argv, "vins_estimator");

    ros::NodeHandle n("~");
    ros::console::set_logger_level(ROSCONSOLE_DEFAULT_NAME, ros::console::levels::Info);

    readParameters(n);  //读取参数
    estimator.setParameter();
#ifdef EIGEN_DONT_PARALLELIZE
    ROS_DEBUG("EIGEN_DONT_PARALLELIZE");
#endif
    ROS_WARN("waiting for image and imu...");

    registerPub(n);

    ros::Subscriber sub_imu = n.subscribe(IMU_TOPIC, 2000, imu_callback, ros::TransportHints().tcpNoDelay());  //订阅了IMU数据topic,从这里获取IMU数据

    ros::Subscriber sub_image = n.subscribe("/feature_tracker/feature", 2000, feature_callback);
    ros::Subscriber sub_restart = n.subscribe("/feature_tracker/restart", 2000, restart_callback);
    ros::Subscriber sub_relo_points = n.subscribe("/pose_graph/match_points", 2000, relocalization_callback);

    std::thread measurement_process{process};
    ros::spin();

    return 0;
}
