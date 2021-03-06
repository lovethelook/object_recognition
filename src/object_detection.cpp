#include <cstdio>
#include <vector>
#include <string>
#include <ros/ros.h>
#include <boost/thread.hpp>
#include <sensor_msgs/PointCloud2.h>
#include <tf/transform_listener.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl_ros/point_cloud.h>
#include <pcl_ros/transforms.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/filters/passthrough.h>
#include <opencv2/opencv.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/imgproc/types_c.h>
#include <image_transport/image_transport.h>
#include <cv_bridge/cv_bridge.h>
#include <pcl/filters/crop_box.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl_ros/point_cloud.h>
#include <pcl/filters/statistical_outlier_removal.h>

#include <robot_msgs/imagePosition.h>
#include <pcl/common/centroid.h>
typedef pcl::PCLPointCloud2 Cloud2;
typedef pcl::PointXYZRGB Point;
typedef pcl::PointCloud<Point> Cloud;
typedef pcl::PointXYZHSV PointHSV;
typedef pcl::PointCloud<PointHSV> CloudHSV;

//#define USE_DEBUG

#ifdef USE_DEBUG
    #define DEBUG(X) X
    #define DCB     //Debug Code Block
#else
    #define DEBUG(X)
#endif

class object_detection{
public:
    object_detection() :
        it_(nh_)
    {
        hsvRanges_.resize(6);
        loadParams();

        pcl_sub_ = nh_.subscribe("/camera/depth_registered/points", 1, &object_detection::pointCloudCB, this);

        //img_sub_ = it_.subscribe("/camera/rgb/image_rect_color", 1, &object_detection::imageCB, this);

        imgPosition_pub_ = nh_.advertise<robot_msgs::imagePosition>("/object_detection/object_position",1);
        img_pub_ = it_.advertise("/object_detection/object",1);

        currentCloudPtr_ = Cloud::Ptr(new Cloud);

#ifdef DCB
            pcl_tf_pub_ = nh_.advertise<sensor_msgs::PointCloud2>("/object_detection/transformed", 1);
            selectedHsvRange_ = 0;
            lastHsvRange_ = -1;
            cv::namedWindow("HSV filter", CV_WINDOW_AUTOSIZE);
            cv::namedWindow("Flood mask", CV_WINDOW_AUTOSIZE);
            cv::namedWindow("Depth filter", CV_WINDOW_AUTOSIZE);
            cv::namedWindow("Blurred image", CV_WINDOW_AUTOSIZE);
            cv::namedWindow("Combined filter", CV_WINDOW_AUTOSIZE);
            setupHsvTrackbars();
            uiThread = boost::thread(&object_detection::asyncImshow, this);
#endif
    }

    ~object_detection() {
#ifdef DCB
            uiThread.interrupt();
            uiThread.join();
#endif
    }

    void imageCB(const sensor_msgs::ImageConstPtr& img_msg) {
        DEBUG(std::cout << "Got image callback" << std::endl;)
        cv_bridge::CvImagePtr cvPtr;
        try {
            cvPtr = cv_bridge::toCvCopy(img_msg, "bgr8");
        }
        catch (cv_bridge::Exception& e) {
            ROS_ERROR("cv_bridge exception: %s", e.what());
            return;
        }
        currentImagePtr_ = cvPtr;
        rows_ = cvPtr->image.rows;
        cols_ = cvPtr->image.cols;
        haveImage_ = true;
    }

    void pointCloudCB(const sensor_msgs::PointCloud2ConstPtr& pclMsg) {
        DEBUG(std::cout << "Got pcl callback" << std::endl;)

        pcl::fromROSMsg(*pclMsg, *currentCloudPtr_);
        tf::StampedTransform transform;
        try {
            tf_sub_.lookupTransform("robot_center", "camera_rgb_optical_frame", ros::Time(0), transform);
        } catch (tf::TransformException ex){
            ROS_ERROR("%s",ex.what());
            return;
        }
        pcl_ros::transformPointCloud(*currentCloudPtr_, *currentCloudPtr_, transform);
        currentCloudPtr_->header.frame_id = "robot_center";

        sensor_msgs::PointCloud2 msgOut;
        pcl::toROSMsg(*currentCloudPtr_, msgOut);
        //Getting Image from the Cloud
        sensor_msgs::Image tempImageMsg;
        pcl::toROSMsg(msgOut,tempImageMsg);
        currentImagePtr_= cv_bridge::toCvCopy(tempImageMsg, "bgr8");
        haveImage_=true;
        currentheader_ = pclMsg->header;
        rows_ = currentImagePtr_->image.rows;
        cols_ = currentImagePtr_->image.cols;
        havePcl_ = true;

#ifdef DCB
        //sensor_msgs::PointCloud2 msgOut;
        pcl::toROSMsg(*currentCloudPtr_, msgOut);
        pcl_tf_pub_.publish(pclMsg);
#endif

    }

    void detect() {
        if(!haveImage_ || !havePcl_) {
            DEBUG(std::cout << "No PCL or image set" << std::endl;)
            return;
        }

        cv::Mat depthMaskIncluded = cv::Mat::zeros(rows_, cols_, CV_8UC1);
        cv::Mat depthMaskExcluded = cv::Mat::zeros(rows_, cols_, CV_8UC1);
        std::vector<int> indices;
        cropDepthData(indices, true);
        for(size_t i = 0; i < indices.size(); ++i) {
            depthMaskIncluded.at<char>(indices[i]) = 255;
        }

        indices.clear();
        cropDepthData(indices, false);
        for(size_t i = 0; i < indices.size(); ++i) {
            depthMaskExcluded.at<char>(indices[i]) = 255;
        }

        cv::Mat HSVmask;
        cv::Mat blurredImage;
        cv::Mat combinedMask;
        cv::medianBlur(currentImagePtr_->image, blurredImage, 9);

#ifdef DCB
        //cv::imshow("Depth filter", depthMaskExcluded);
        cv::imshow("Blurred image", blurredImage);
        cv::Mat savedHSVMask;
        cv::Mat saveCombinedMask;
#endif
        cv::cvtColor(blurredImage, blurredImage, CV_BGR2HSV);

        double largestArea = 0;
        int largestIndex = 0;
        std::string largestAreaColor = "";
        std::vector<cv::Point> largestContour;
        cv::Mat contourMask;
        for(size_t i = 0; i < hsvRanges_.size(); ++i) {
            cv::inRange(blurredImage, hsvRanges_[i].min, hsvRanges_[i].max, HSVmask);

#ifdef DCB
            if(i == selectedHsvRange_) {
               cv::imshow("HSV filter", HSVmask);
            }
#endif

            if(hsvRanges_[i].inverted) {
                cv::bitwise_not(HSVmask, HSVmask);
            }

            if(hsvRanges_[i].includeInvalid) {
                combinedMask = HSVmask & depthMaskIncluded;
            } else {
                combinedMask = HSVmask & depthMaskExcluded;
            }

            std::vector<std::vector<cv::Point> > contours;
            std::vector<cv::Vec4i> notUsedHierarchy;

            contourMask = combinedMask.clone();
            cv::findContours(contourMask, contours, notUsedHierarchy, CV_RETR_LIST, CV_CHAIN_APPROX_NONE);
            if(contours.size() > 0) {
                for(size_t j = 0; j < contours.size(); ++j) {
                    double area = cv::contourArea(contours[j]);
                    if(area > areaMinThreshold_ && area > largestArea && area < areaMaxThreshold_ ) {
                        largestArea = area;
                        largestAreaColor = hsvRanges_[i].color;
                        largestContour = contours[j];
                        largestIndex = i;
#ifdef DCB
                        savedHSVMask = HSVmask.clone();
                        saveCombinedMask = combinedMask.clone();
#endif

                    }
                }
            }
        }

        if(largestContour.size() == 0) {
            return;
        }

#ifdef DCB
        //cv::imshow("HSV filter", savedHSVMask);
        cv::imshow("Combined filter", saveCombinedMask);
        std::cout << "Color filter used: " << largestAreaColor << std::endl;
#endif

        //Getting the Position of the largest Contour
        Cloud objectCloud;
        cv::Point2f point;
        //DEBUG(std::cout<< " Try to get a Pointcloud in the largest Contour" << std::endl;)
        //cv::Mat contourMask  = cv::Mat::zeros(rows_, cols_, CV_8U);
        int debug=0;

        for(int x=0;x<cols_; x++){
            for(int y=0;y<rows_; y++){
                point.x=x;
                point.y=y;


                if(0 <= cv::pointPolygonTest(largestContour,point,false)){
                    //contourMask.at<char>(x,y)=255;
                    objectCloud.points.push_back(currentCloudPtr_->at(x,y));

//                    DEBUG(std::cout<< "Pointclouddata:  "<< currentCloudPtr_->at(x,y) << std::endl;)
                    debug++;
                }
            }
        }

        DEBUG(std::cout<< "Got Pointcloud with " << objectCloud.points.size() << "  Points " << std::endl;)
        Eigen::Vector4f massCenter;
        std::vector<int> indicies;
        Cloud withoutNan;
        objectCloud.is_dense=false;
        pcl::removeNaNFromPointCloud(objectCloud,withoutNan,indicies);
        DEBUG(std::cout<< "Got Pointcloud with "<< withoutNan.points.size()  << "  Points after removing NaN" << std::endl;)
	if(withoutNan.points.size() < 50){
		return;
	}
        pcl::compute3DCentroid(objectCloud, massCenter);

        DEBUG(std::cout<< "Got massCenter " << massCenter<<std::endl;)


       // -------------------------- Trying to see if the area of an circle or box around the contour is smaller --
        cv::Point2f centermincircle;
        float radiusmincircle;
        cv::minEnclosingCircle(cv::Mat(largestContour), centermincircle , radiusmincircle);
        double areamincircle= pow(radiusmincircle,2) * 3.141592653;

        cv::RotatedRect minimumrect = cv::minAreaRect(cv::Mat(largestContour));
        double areaminrectangle = minimumrect.size.area();
        if(areamincircle<areaminrectangle){
            std::cout << " It is probably A "<< largestAreaColor<<" CIRCLE !!!!! " << std::endl;

        }
        else{
             std::cout << " It is probably A "<< largestAreaColor<<" RECTANGLE !!!!! " << std::endl;
        }

        cv::Rect objRect = cv::boundingRect(largestContour);

#ifdef DCB
        cv::Point objCenter(objRect.x + objRect.width/2, objRect.y + objRect.height/2);
        cv::Mat floodMask = cv::Mat::zeros(rows_+2, cols_+2, CV_8UC1);
        hsvRange& cr = hsvRanges_[largestIndex];
        cv::Scalar upDiff(cr.updiffh, cr.updiffs, cr.updiffv);
        cv::Scalar lowDiff(cr.lowdiffh, cr.lowdiffs, cr.lowdiffv);
        cv::floodFill(blurredImage, floodMask, objCenter, cv::Scalar(0, 0, 0), NULL, lowDiff, upDiff, 8 | CV_FLOODFILL_FIXED_RANGE | CV_FLOODFILL_MASK_ONLY | 255 << 8);
        cv::medianBlur(floodMask, floodMask, 3);
        cv::circle(floodMask, objCenter, 3, cv::Scalar(128, 0, 0), 2);
        //cv::imshow("Flood mask", floodMask);
#endif

        objRect.x = std::max(0, objRect.x - rectPadding_);
        objRect.y = std::max(0, objRect.y - rectPadding_);
        objRect.height = std::min(rows_ - objRect.y, objRect.height + 2*rectPadding_ + heightCorrection_);
        objRect.width = std::min(cols_ - objRect.x, objRect.width + 2*rectPadding_);
        cv::Mat objImgOut = currentImagePtr_->image(objRect);

        geometry_msgs::Point dir_msg_out;
        dir_msg_out.x = massCenter[0];
        dir_msg_out.y = massCenter[1];
        dir_msg_out.z = massCenter[2];

        cv_bridge::CvImage img;
        img.image = objImgOut;
        sensor_msgs::ImagePtr imgOut = img.toImageMsg();
        imgOut->encoding = "bgr8";
        robot_msgs::imagePosition msgOut;

        msgOut.header = currentheader_;
        msgOut.color=largestAreaColor;
        msgOut.point=dir_msg_out;
        msgOut.image = imgOut.operator *();
        imgPosition_pub_.publish(msgOut);
        img_pub_.publish(imgOut);
        DEBUG(std::cout<< "Sending Completed " << std::endl;)
    }


private:
    class hsvRange {
    public:

        hsvRange() :
            hmin(min[0]), smin(min[1]), vmin(min[2]),
            hmax(max[0]), smax(max[1]), vmax(max[2])
        {
        }

        hsvRange(const hsvRange& other) :
            hmin(min[0]), smin(min[1]), vmin(min[2]),
            hmax(max[0]), smax(max[1]), vmax(max[2])
        {
            min = other.min;
            max = other.max;
        }

        hsvRange& operator=(const hsvRange& other) {
            min = other.min;
            max = other.max;
        }

        template <typename T> void setValues(const T& hmin, const T& smin, const T& vmin,
                                             const T& hmax, const T& smax, const T& vmax) {
            this->hmin = hmin;
            this->smin = smin;
            this->vmin = vmin;
            this->hmax = hmax;
            this->smax = smax;
            this->vmax = vmax;
        }

        std::string color;

        cv::Scalar min;
        cv::Scalar max;

        bool inverted;
	bool includeInvalid;
        double& hmin;
        double& smin;
        double& vmin;
        double& hmax;
        double& smax;
        double& vmax;
        double updiffh, updiffs, updiffv;
        double lowdiffh, lowdiffs, lowdiffv;
    };

    void cropDepthData(std::vector<int>& outIndices, bool includeInvalid =true) {
        for(int index = 150*cols_; index < rows_*cols_; ++index) {
            Point& cp = currentCloudPtr_->at(index);
            if(includeInvalid && isnan(cp.x)) {
                outIndices.push_back(index);
            } else if(
                    cp.y >= cbMin_[1] &&
                    cp.y <= cbMax_[1] &&
                    cp.x >= cbMin_[0] &&
                    cp.x <= cbMax_[0] &&
                    cp.z >= cbMin_[2] &&
                    cp.z <= cbMax_[2]) {
                outIndices.push_back(index);
            }
        }
    }

    void loadParams(){
        getParam("object_detection/crop/wMin", cbMin_[0], -10);
        getParam("object_detection/crop/dMin", cbMin_[1], -10);
        getParam("object_detection/crop/hMin", cbMin_[2], -10);
        getParam("object_detection/crop/wMax", cbMax_[0], 10);
        getParam("object_detection/crop/dMax", cbMax_[1], 10);
        getParam("object_detection/crop/hMax", cbMax_[2], 10);

        getParam("object_detection/voxel/leafsize", voxelsize_, 0.005);
        getParam("object_detection/rectPadding", rectPadding_, 5);

        getParam("object_detection/heightCorrection", heightCorrection_, 10);
        getParam("object_detection/minArea", areaMinThreshold_, 1000);
        getParam("object_detection/maxArea", areaMaxThreshold_, 6000);


        std::string hsvParamName("object_detection/hsv");
        for(size_t i = 0; i < hsvRanges_.size(); ++i) {
            std::stringstream ss; ss << hsvParamName << i;
            getParam(ss.str() + "/hmin", hsvRanges_[i].hmin, 10);
            getParam(ss.str() + "/smin", hsvRanges_[i].smin, 10);
            getParam(ss.str() + "/vmin", hsvRanges_[i].vmin, 10);
            getParam(ss.str() + "/hmax", hsvRanges_[i].hmax, 180);
            getParam(ss.str() + "/smax", hsvRanges_[i].smax, 255);
            getParam(ss.str() + "/vmax", hsvRanges_[i].vmax, 255);

            getParam(ss.str() + "/updiffh", hsvRanges_[i].updiffh, 0);
            getParam(ss.str() + "/updiffs", hsvRanges_[i].updiffs, 0);
            getParam(ss.str() + "/updiffv", hsvRanges_[i].updiffv, 0);

            getParam(ss.str() + "/lowdiffh", hsvRanges_[i].lowdiffh, 0);
            getParam(ss.str() + "/lowdiffs", hsvRanges_[i].lowdiffs, 0);
            getParam(ss.str() + "/lowdiffv", hsvRanges_[i].lowdiffv, 0);

            getParam(ss.str() + "/color", hsvRanges_[i].color, "");
            getParam(ss.str() + "/inverted", hsvRanges_[i].inverted, false);
            getParam(ss.str() + "/includeinvalid", hsvRanges_[i].includeInvalid, true);

        }
    }

    template <typename T1, typename T2> bool getParam(const std::string paramName, T1& variable, const T2& standardValue) {
        if(nh_.hasParam(paramName)) {
            nh_.getParam(paramName, variable);
            return true;
        }
        variable = standardValue;
        DEBUG(std::cout << "Parameter '" << paramName << "' not found" << std::endl;)
        return false;
    }

    template <typename T> bool getParam(const std::string paramName, float& variable, const T& standardValue) {
        if(nh_.hasParam(paramName)) {
            double temp;
            nh_.getParam(paramName, temp);
            variable = temp;
            return true;
        }
        variable = standardValue;
        DEBUG(std::cout << paramName << "not found" << std::endl;)
        return false;
    }

#ifdef DCB
        //Separate thread to update the UI when debugging
        void asyncImshow() {
            ros::Rate rate(5);
            while(1) {
                try {
                    boost::this_thread::interruption_point();
                    updateHsvTrackbars();
                    cv::waitKey(1);
                    rate.sleep();
                } catch(boost::thread_interrupted&){
                    return;
                }
            }
        }

        //Setup all the UI trackbars when debugging
        void setupHsvTrackbars() {
            cv::namedWindow("HSVTrackbars",CV_WINDOW_NORMAL);
            cv::createTrackbar("Index", "HSVTrackbars", &selectedHsvRange_, hsvRanges_.size()-1);
            cv::createTrackbar("Hmin", "HSVTrackbars", NULL, 180);
            cv::createTrackbar("Hmax", "HSVTrackbars", NULL, 180);
            cv::createTrackbar("Smin", "HSVTrackbars", NULL, 255);
            cv::createTrackbar("Smax", "HSVTrackbars", NULL, 255);
            cv::createTrackbar("Vmin", "HSVTrackbars", NULL, 255);
            cv::createTrackbar("Vmax", "HSVTrackbars", NULL, 255);

            cv::createTrackbar("updiffh", "HSVTrackbars", NULL, 25500);
            cv::createTrackbar("updiffs", "HSVTrackbars", NULL, 25500);
            cv::createTrackbar("updiffv", "HSVTrackbars", NULL, 25500);
            cv::createTrackbar("lowdiffh", "HSVTrackbars", NULL, 25500);
            cv::createTrackbar("lowdiffs", "HSVTrackbars", NULL, 25500);
            cv::createTrackbar("lowdiffv", "HSVTrackbars", NULL, 25500);
        }

        //Read values from trackbars when debugging
        void updateHsvTrackbars() {
            selectedHsvRange_ = cv::getTrackbarPos("Index", "HSVTrackbars");
            if(lastHsvRange_ != selectedHsvRange_) {
                lastHsvRange_ = selectedHsvRange_;
                cv::setTrackbarPos("Hmin", "HSVTrackbars", hsvRanges_[selectedHsvRange_].hmin);
                cv::setTrackbarPos("Hmax", "HSVTrackbars", hsvRanges_[selectedHsvRange_].hmax);
                cv::setTrackbarPos("Smin", "HSVTrackbars", hsvRanges_[selectedHsvRange_].smin);
                cv::setTrackbarPos("Smax", "HSVTrackbars", hsvRanges_[selectedHsvRange_].smax);
                cv::setTrackbarPos("Vmin", "HSVTrackbars", hsvRanges_[selectedHsvRange_].vmin);
                cv::setTrackbarPos("Vmax", "HSVTrackbars", hsvRanges_[selectedHsvRange_].vmax);
            }

            hsvRanges_[selectedHsvRange_].hmin = cv::getTrackbarPos("Hmin", "HSVTrackbars");
            hsvRanges_[selectedHsvRange_].hmax = cv::getTrackbarPos("Hmax", "HSVTrackbars");
            hsvRanges_[selectedHsvRange_].smin = cv::getTrackbarPos("Smin", "HSVTrackbars");
            hsvRanges_[selectedHsvRange_].smax = cv::getTrackbarPos("Smax", "HSVTrackbars");
            hsvRanges_[selectedHsvRange_].vmin = cv::getTrackbarPos("Vmin", "HSVTrackbars");
            hsvRanges_[selectedHsvRange_].vmax = cv::getTrackbarPos("Vmax", "HSVTrackbars");

            updiffh_ = cv::getTrackbarPos("updiffh", "HSVTrackbars")/100.0;
            updiffs_ = cv::getTrackbarPos("updiffs", "HSVTrackbars")/100.0;
            updiffv_ = cv::getTrackbarPos("updiffv", "HSVTrackbars")/100.0;
            lowdiffh_ = cv::getTrackbarPos("lowdiffh", "HSVTrackbars")/100.0;
            lowdiffs_ = cv::getTrackbarPos("lowdiffs", "HSVTrackbars")/100.0;
            lowdiffv_ = cv::getTrackbarPos("lowdiffv", "HSVTrackbars")/100.0;
        }
#endif


    ros::NodeHandle nh_;
    ros::Subscriber pcl_sub_;
    image_transport::ImageTransport it_;
    image_transport::Subscriber img_sub_;
    image_transport::Publisher img_pub_;
    tf::TransformListener tf_sub_;
    ros::Publisher imgPosition_pub_;
    bool haveImage_;
    bool havePcl_;
    std_msgs::Header currentheader_;
    double voxelsize_;
    double areaMinThreshold_ , areaMaxThreshold_;
    double updiffh_, updiffs_, updiffv_, lowdiffh_, lowdiffs_, lowdiffv_;
    int rectPadding_;
    int rows_, cols_;
    int selectedHsvRange_;
    int lastHsvRange_;
    int heightCorrection_;
    int hmin_, smin_, vmin_, hmax_, smax_, vmax_;
    Eigen::Vector4f cbMin_, cbMax_;
    std::vector<hsvRange> hsvRanges_;

    Cloud::Ptr currentCloudPtr_;
    cv_bridge::CvImagePtr currentImagePtr_;

#ifdef DCB
    ros::Publisher pcl_tf_pub_;
    boost::thread uiThread;
#endif

};



int main(int argc, char** argv){
    ros::init(argc, argv, "object_detection");
    object_detection od;

    ros::Rate rate(5);
    while(ros::ok()) {
        ros::spinOnce();
        od.detect();
        rate.sleep();
    }

}

