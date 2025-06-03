#include "PDFViewerPage.h"
#include <System.h>  // ORB-SLAM2头文件
#include <opencv2/calib3d.hpp>
#include <mutex>

// ORB-SLAM2集成类
class ORBSLAM2Integration {
public:
    ORBSLAM2Integration(const std::string& vocabPath, const std::string& settingsPath) {
        // 初始化ORB-SLAM2
        mpSLAM = new ORB_SLAM2::System(vocabPath, settingsPath, ORB_SLAM2::System::MONOCULAR, true);
        mbInitialized = false;
        mLastFrameId = 0;

        // 平面检测参数
        mMinInliers = 100;       // 最小内点数量
        mPlaneThreshold = 0.01f; // 平面拟合阈值（米）
        mMinPlaneSize = 0.2f;    // 最小平面尺寸（占总面积比例）

        // 为桌面重新检测设置初始值
        mLostFrames = 0;
        mMaxLostFrames = 30;     // 丢失30帧后重新检测
    }

    ~ORBSLAM2Integration() {
        // 关闭并保存SLAM系统
        if (mpSLAM) {
            mpSLAM->Shutdown();
            delete mpSLAM;
            mpSLAM = nullptr;
        }
    }
    
    // 处理新帧
    bool processFrame(const cv::Mat& frame, std::vector<cv::Point>& deskContour, cv::Mat& pose) {
        if (!mpSLAM) return false;
        
        // 增加帧ID
        mLastFrameId++;
        
        // 传递帧到ORB-SLAM2
        cv::Mat Tcw = mpSLAM->TrackMonocular(frame, mLastFrameId * 0.033); // 假设30fps
        
        // 检查跟踪状态
        ORB_SLAM2::Tracking::eTrackingState state = mpSLAM->GetTrackingState();
        
        if (state == ORB_SLAM2::Tracking::OK) {
            // 重置丢失帧计数
            mLostFrames = 0;

            // 如果我们有一个好的姿态，保存它
            if (!Tcw.empty()) {
                std::lock_guard<std::mutex> lock(mMutexPose);
                mCurrentPose = Tcw.clone();
                pose = Tcw.clone();
                
                // 如果尚未初始化或需要刷新桌面检测
                if (!mbInitialized || mFramesSinceLastDetection > 100) {
                    detectDesk(frame, deskContour);
                    mbInitialized = true;
                    mFramesSinceLastDetection = 0;
                } else {
                    // 使用当前姿态更新桌面轮廓
                    updateDeskContour(deskContour);
                    mFramesSinceLastDetection++;
                }
                
                return true;
            }
        } else {
            // 跟踪丢失
            mLostFrames++;
            
            // 如果连续丢失太多帧，重置初始化标志
            if (mLostFrames > mMaxLostFrames) {
                mbInitialized = false;
                mDeskPlaneCoefficients = cv::Mat();
                deskContour.clear();
                
                // 尝试重新定位
                if (state == ORB_SLAM2::Tracking::LOST) {
                    mpSLAM->ForceRelocalisation();
                }
                
                return false;
            } else if (!mCurrentPose.empty()) {
                // 使用上一个有效的姿态继续渲染
                std::lock_guard<std::mutex> lock(mMutexPose);
                pose = mCurrentPose.clone();
                updateDeskContour(deskContour);
                return true;
            }
        }
        
        return false;
    }

    // 检测桌面平面并提取轮廓
    bool detectDesk(const cv::Mat& frame, std::vector<cv::Point>& deskContour) {
        if (!mpSLAM) return false;
        
        // 获取地图点
        std::vector<ORB_SLAM2::MapPoint*> mapPoints = mpSLAM->GetTrackedMapPoints();
        std::vector<cv::Mat> pointPositions;
        std::vector<cv::Point2f> imagePoints;
        
        // 获取当前帧
        ORB_SLAM2::Frame currentFrame = mpSLAM->GetCurrentFrame();
        
        // 收集有效的3D地图点及其在图像中的投影
        for (size_t i = 0; i < mapPoints.size(); i++) {
            ORB_SLAM2::MapPoint* mp = mapPoints[i];
            if (mp && !mp->isBad()) {
                cv::Mat pos = mp->GetWorldPos();
                pointPositions.push_back(pos);
                
                // 获取地图点在图像中的位置
                float u, v;
                bool visible = currentFrame.isInFrustum(mp, 0.5);
                if (visible) {
                    currentFrame.ProjectPointUndistorted(mp->GetWorldPos(), u, v);
                    imagePoints.push_back(cv::Point2f(u, v));
                }
            }
        }
        
        // 如果点太少，无法检测平面
        if (pointPositions.size() < 10) {
            return false;
        }
        
        // 将点转换为OpenCV格式
        cv::Mat points3D(pointPositions.size(), 3, CV_32F);
        for (size_t i = 0; i < pointPositions.size(); i++) {
            points3D.at<float>(i, 0) = pointPositions[i].at<float>(0);
            points3D.at<float>(i, 1) = pointPositions[i].at<float>(1);
            points3D.at<float>(i, 2) = pointPositions[i].at<float>(2);
        }
        
        // 使用RANSAC检测平面
        std::vector<int> inliers;
        cv::Mat planeCoeffs = ransacPlane(points3D, inliers, mPlaneThreshold, mMinInliers);
        
        if (planeCoeffs.empty() || inliers.size() < mMinInliers) {
            return false;
        }
        
        // 保存平面系数
        mDeskPlaneCoefficients = planeCoeffs.clone();
        
        // 提取平面上的点
        std::vector<cv::Point2f> planePointsImage;
        for (int idx : inliers) {
            if (idx < imagePoints.size()) {
                planePointsImage.push_back(imagePoints[idx]);
            }
        }
        
        // 如果平面上的点太少，无法提取轮廓
        if (planePointsImage.size() < 4) {
            return false;
        }
        
        // 计算平面点的边界框和轮廓
        cv::Rect boundRect = cv::boundingRect(planePointsImage);
        
        // 使用凸包获得更准确的边界
        std::vector<cv::Point2f> hullPoints;
        cv::convexHull(planePointsImage, hullPoints);
        
        // 简化轮廓到四边形
        std::vector<cv::Point2f> quadPoints;
        approximateQuadrilateral(hullPoints, quadPoints);
        
        // 返回桌面四边形轮廓
        deskContour.clear();
        for (const auto& p : quadPoints) {
            deskContour.push_back(cv::Point(p.x, p.y));
        }
        
        // 验证轮廓大小和形状
        if (validateDeskContour(deskContour, frame.size())) {
            // 保存参考轮廓用于后续跟踪
            mReferenceDeskContour = deskContour;
            mReferenceImageSize = frame.size();
            return true;
        }
        
        deskContour.clear();
        return false;
    }

private:
    // RANSAC平面拟合
    cv::Mat ransacPlane(const cv::Mat& points, std::vector<int>& inliers, float threshold, int minInliers) {
        if (points.empty() || points.rows < 3) {
            return cv::Mat();
        }
        
        const int iterations = 100;
        std::vector<int> bestInliers;
        cv::Mat bestPlane;
        
        // RANSAC迭代
        for (int i = 0; i < iterations; i++) {
            // 随机选择3个点
            std::vector<int> samples(3);
            cv::RNG rng(cv::getTickCount());
            for (int j = 0; j < 3; j++) {
                samples[j] = rng.uniform(0, points.rows);
            }
            
            // 确保选择了不同的点
            if (samples[0] == samples[1] || samples[0] == samples[2] || samples[1] == samples[2]) {
                continue;
            }
            
            // 计算平面方程 Ax + By + Cz + D = 0
            cv::Mat p1 = points.row(samples[0]);
            cv::Mat p2 = points.row(samples[1]);
            cv::Mat p3 = points.row(samples[2]);
            
            cv::Mat v1 = p2 - p1;
            cv::Mat v2 = p3 - p1;
            
            // 计算法向量 (A,B,C) = v1 x v2
            cv::Mat normal = v1.cross(v2);
            cv::normalize(normal, normal);
            
            // 计算D
            float D = -normal.dot(p1);
            
            // 创建平面系数 [A,B,C,D]
            cv::Mat plane = cv::Mat::zeros(4, 1, CV_32F);
            normal.copyTo(plane(cv::Rect(0, 0, 1, 3)));
            plane.at<float>(3, 0) = D;
            
            // 计算内点
            std::vector<int> currentInliers;
            for (int j = 0; j < points.rows; j++) {
                cv::Mat p = points.row(j);
                float distance = std::abs(normal.dot(p) + D);
                if (distance < threshold) {
                    currentInliers.push_back(j);
                }
            }
            
            // 更新最佳结果
            if (currentInliers.size() > bestInliers.size() && currentInliers.size() >= minInliers) {
                bestInliers = currentInliers;
                bestPlane = plane.clone();
            }
        }
        
        inliers = bestInliers;
        return bestPlane;
    }
    
    // 近似四边形轮廓
    void approximateQuadrilateral(const std::vector<cv::Point2f>& hull, std::vector<cv::Point2f>& quad) {
        quad.clear();
        
        if (hull.size() < 4) {
            return;
        }
        
        // 计算凸包的周长
        double perimeter = 0;
        for (size_t i = 0; i < hull.size(); i++) {
            size_t j = (i + 1) % hull.size();
            perimeter += cv::norm(hull[i] - hull[j]);
        }
        
        // 使用多边形近似
        std::vector<cv::Point2f> approxCurve;
        double epsilon = 0.05 * perimeter;
        cv::approxPolyDP(hull, approxCurve, epsilon, true);
        
        // 如果近似结果是四边形，直接使用
        if (approxCurve.size() == 4) {
            quad = approxCurve;
            return;
        }
        
        // 否则，寻找四个最远的点作为四边形的角点
        findFourCorners(hull, quad);
    }
    
    // 查找四个角点
    void findFourCorners(const std::vector<cv::Point2f>& hull, std::vector<cv::Point2f>& quad) {
        quad.clear();
        
        if (hull.size() < 4) {
            return;
        }
        
        // 找到最左、最右、最上、最下的点
        cv::Point2f leftmost(FLT_MAX, 0);
        cv::Point2f rightmost(0, 0);
        cv::Point2f topmost(0, FLT_MAX);
        cv::Point2f bottommost(0, 0);
        
        for (const auto& p : hull) {
            if (p.x < leftmost.x) leftmost = p;
            if (p.x > rightmost.x) rightmost = p;
            if (p.y < topmost.y) topmost = p;
            if (p.y > bottommost.y) bottommost = p;
        }
        
        quad.push_back(topmost);
        quad.push_back(rightmost);
        quad.push_back(bottommost);
        quad.push_back(leftmost);
    }
    
    // 验证桌面轮廓
    bool validateDeskContour(const std::vector<cv::Point>& contour, const cv::Size& imageSize) {
        if (contour.size() != 4) {
            return false;
        }
        
        // 计算轮廓面积
        double area = cv::contourArea(contour);
        double imageArea = imageSize.width * imageSize.height;
        
        // 检查轮廓是否至少占图像面积的一定比例
        if (area < mMinPlaneSize * imageArea) {
            return false;
        }
        
        // 验证是否是凸四边形
        if (!cv::isContourConvex(contour)) {
            return false;
        }
        
        // 计算长宽比，检查是否合理（排除极端情况）
        cv::RotatedRect rect = cv::minAreaRect(contour);
        float aspectRatio = rect.size.width / rect.size.height;
        if (aspectRatio < 1.0f) aspectRatio = 1.0f / aspectRatio;
        
        // 合理的桌面长宽比范围
        if (aspectRatio > 5.0f || aspectRatio < 0.2f) {
            return false;
        }
        
        return true;
    }
    
    // 使用当前位姿更新桌面轮廓
    void updateDeskContour(std::vector<cv::Point>& contour) {
        if (mReferenceDeskContour.empty() || mDeskPlaneCoefficients.empty() || mCurrentPose.empty()) {
            return;
        }
        
        // 获取参考相机位姿的逆
        cv::Mat Twc_ref = mReferencePose.inv();
        
        // 计算当前相机位姿相对于参考位姿的变换
        cv::Mat relative_transform = mCurrentPose * Twc_ref;
        
        // 将参考桌面的3D点从世界坐标变换到当前相机坐标
        std::vector<cv::Point3f> desk3DPoints;
        
        // 从平面方程和参考轮廓重建3D点
        float a = mDeskPlaneCoefficients.at<float>(0, 0);
        float b = mDeskPlaneCoefficients.at<float>(1, 0);
        float c = mDeskPlaneCoefficients.at<float>(2, 0);
        float d = mDeskPlaneCoefficients.at<float>(3, 0);
        
        // 假设相机内参
        cv::Mat K = (cv::Mat_<float>(3, 3) << 
            mReferenceImageSize.width, 0, mReferenceImageSize.width/2,
            0, mReferenceImageSize.height, mReferenceImageSize.height/2,
            0, 0, 1);
        
        // 反投影参考桌面轮廓点到3D空间
        for (const auto& p : mReferenceDeskContour) {
            // 归一化坐标
            cv::Point3f ray(
                (p.x - K.at<float>(0, 2)) / K.at<float>(0, 0),
                (p.y - K.at<float>(1, 2)) / K.at<float>(1, 1),
                1.0f
            );
            
            // 与平面相交的射线方程
            float t = -d / (a * ray.x + b * ray.y + c * ray.z);
            cv::Point3f point3D(ray.x * t, ray.y * t, ray.z * t);
            
            desk3DPoints.push_back(point3D);
        }
        
        // 将3D点投影到当前图像
        std::vector<cv::Point2f> projectedPoints;
        
        for (const auto& p3d : desk3DPoints) {
            // 转换到齐次坐标
            cv::Mat p4d = (cv::Mat_<float>(4, 1) << p3d.x, p3d.y, p3d.z, 1.0f);
            
            // 应用相对变换
            cv::Mat p_cam = relative_transform * p4d;
            
            // 透视除法
            float x = p_cam.at<float>(0, 0) / p_cam.at<float>(2, 0);
            float y = p_cam.at<float>(1, 0) / p_cam.at<float>(2, 0);
            
            // 应用相机内参
            float u = K.at<float>(0, 0) * x + K.at<float>(0, 2);
            float v = K.at<float>(1, 1) * y + K.at<float>(1, 2);
            
            projectedPoints.push_back(cv::Point2f(u, v));
        }
        
        // 更新输出轮廓
        contour.clear();
        for (const auto& p : projectedPoints) {
            contour.push_back(cv::Point(p.x, p.y));
        }
    }

private:
    ORB_SLAM2::System* mpSLAM;         // ORB-SLAM2系统指针
    bool mbInitialized;                 // 初始化标志
    unsigned long mLastFrameId;         // 上一帧ID
    cv::Mat mCurrentPose;               // 当前相机位姿
    cv::Mat mReferencePose;             // 参考位姿（桌面检测时的位姿）
    cv::Mat mDeskPlaneCoefficients;     // 桌面平面方程系数
    std::vector<cv::Point> mReferenceDeskContour; // 参考桌面轮廓
    cv::Size mReferenceImageSize;       // 参考图像大小
    std::mutex mMutexPose;              // 位姿互斥锁
    
    // 参数
    int mMinInliers;                    // 最小内点数
    float mPlaneThreshold;              // 平面拟合阈值
    float mMinPlaneSize;                // 最小平面尺寸比例
    int mLostFrames;                    // 连续丢失帧数量
    int mMaxLostFrames;                 // 最大允许丢失帧数量
    int mFramesSinceLastDetection;      // 自上次检测以来的帧数
};