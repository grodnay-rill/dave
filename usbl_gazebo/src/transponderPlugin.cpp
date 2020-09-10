#include <gazebo/gazebo.hh>
#include <gazebo/physics/physics.hh>

#include <ignition/transport/Node.hh>
#include <ignition/math/Pose3.hh>

#include <ros/ros.h>
#include <ros/callback_queue.h>
#include <ros/subscribe_options.h>
#include <std_msgs/String.h>
#include <std_msgs/Float64.h>
#include "usbl_gazebo/USBLResponse.h"
#include "usbl_gazebo/USBLCommand.h"

#include <thread>
#include <random>

namespace gazebo
{
    class TransponderPlugin : public ModelPlugin
    {
        public: TransponderPlugin(): m_temperature(10.0), m_noiseMu(0), m_noiseSigma(1) {}

        public: void Load(physics::ModelPtr _model, sdf::ElementPtr _sdf)
        {
            /***************************************************  SDF PARAMETERS ********************************************************/

            // Ensure ROS is initialized for publishers and subscribers
            if(!ros::isInitialized())
            {
                gzerr << "ROS has not been inintialized\n";
                int argc = 0;
                char** argv = NULL;
                ros::init(argc, argv, "transponder", ros::init_options::NoSigintHandler);
                return;
            }

            /*---------------------------------------------------------------------------------------------------------------------------------*/
            // Grab namespace from SDF
            if(!_sdf->HasElement("namespace"))
            {
                gzerr << "Missing required parameter <namespace>, plugin will not be initialized." << std::endl;
                return;
            }
            this->m_namespace = _sdf->Get<std::string>("namespace");

            /*---------------------------------------------------------------------------------------------------------------------------------*/
            // Obtain transponder device name from SDF
            if(!_sdf->HasElement("transponder_device"))
            {
                gzerr << "Missing required parameter <transponder_device>, plugin will not be initialized." << std::endl;
                return;
            }
            this->m_transponderDevice = _sdf->Get<std::string>("transponder_device");
            gzmsg << "Transponder device: " << this->m_transponderDevice << std::endl;

            /*---------------------------------------------------------------------------------------------------------------------------------*/
            // get transponder ID
            if(!_sdf->HasElement("transponder_ID"))
            {
                gzerr << "Missing required parameter <transponder_ID>, plugin will not be initialized." << std::endl;
                return;
            }
            this->m_transponderID = _sdf->Get<std::string>("transponder_ID");

            /*---------------------------------------------------------------------------------------------------------------------------------*/
            // get transceiver ID
            if(!_sdf->HasElement("transceiver_device"))
            {
                gzerr << "Missing required parameter <transceiver_device>, plugin will not be initialized." << std::endl;
                return;
            }
            this->m_transceiverDevice = _sdf->Get<std::string>("transceiver_device");

            /*---------------------------------------------------------------------------------------------------------------------------------*/
            // get transceiver ID
            if(!_sdf->HasElement("transceiver_ID"))
            {
                gzerr << "Missing required parameter <transceiver_ID>, plugin will not be initialized." << std::endl;
                return;
            }
            this->m_transceiverID = _sdf->Get<std::string>("transceiver_ID");

            /*---------------------------------------------------------------------------------------------------------------------------------*/
            // get the mean of normal distribution for the noise model
            if(_sdf->HasElement("mu"))
            {
                this->m_noiseMu = _sdf->Get<double>("mu");
            }

            /*---------------------------------------------------------------------------------------------------------------------------------*/
            // get the standard deviation of normal distribution for the noise model
            if(_sdf->HasElement("sigma"))
            {
                this->m_noiseSigma = _sdf->Get<double>("sigma");
            }

            // store this entity model
            this->m_model = _model;

            /*****************************************************************************************************************************/

            /************************************************  GAZEBO PUBLISHERS *********************************************************/
            // initialize Gazebo node
            this->m_gzNode = transport::NodePtr(new transport::Node());
            this->m_gzNode->Init();

            // define Gazebo publisher for entity's global position
            this->m_globalPosPub = this->m_gzNode->Advertise<msgs::Vector3d>("/" + this->m_namespace + "/" + this->m_transceiverDevice + "_" + this->m_transponderID + "/global_position");
            /******************************************************************************************************************************/

            /***************************************************  ROS PUBLISHERS *********************************************************/
            this->m_rosNode.reset(new ros::NodeHandle(this->m_transponderDevice));

            std::string commandResponseTopic("/" + this->m_namespace + "/" + this->m_transceiverDevice + "_" + this->m_transceiverID + "/command_response");
            this->m_commandResponsePub = this->m_rosNode->advertise<usbl_gazebo::USBLResponse>(commandResponseTopic, 1);

            /******************************************************************************************************************************/

            /***************************************************  ROS SUBSCRIBERS *********************************************************/

            this->m_rosNode.reset(new ros::NodeHandle(this->m_transponderDevice));

            ros::SubscribeOptions iis_ping =
                ros::SubscribeOptions::create<std_msgs::String>(
                    "/" + this->m_namespace + "/" + this->m_transponderDevice + "_" + this->m_transponderID + "/individual_interrogation_ping",
                    1,
                    boost::bind(&TransponderPlugin::iisRosCallback, this, _1),
                    ros::VoidPtr(), &this->m_rosQueue);

            this->m_iisSub = this->m_rosNode->subscribe(iis_ping);

            ros::SubscribeOptions cis_ping =
                ros::SubscribeOptions::create<std_msgs::String>(
                    "/" + this->m_namespace + "/common_interrogation_ping",
                    1,
                    boost::bind(&TransponderPlugin::cisRosCallback, this, _1),
                    ros::VoidPtr(), &this->m_rosQueue);

            this->m_cisSub = this->m_rosNode->subscribe(cis_ping);

            // create ROS subscriber for temperature
            ros::SubscribeOptions temperature_sub =
                ros::SubscribeOptions::create<std_msgs::Float64>(
                    "/" + this->m_namespace + "/" + this->m_transponderDevice + "_" + this->m_transponderID + "/temperature",
                    1,
                    boost::bind(&TransponderPlugin::temperatureRosCallback, this, _1),
                    ros::VoidPtr(), &this->m_rosQueue);

            this->m_temperatureSub = this->m_rosNode->subscribe(temperature_sub);

            ros::SubscribeOptions command_sub =
                ros::SubscribeOptions::create<usbl_gazebo::USBLCommand>(
                    "/" + this->m_namespace + "/" + this->m_transponderDevice + "_" + this->m_transponderID + "/command_request",
                    1,
                    boost::bind(&TransponderPlugin::commandRosCallback, this, _1),
                    ros::VoidPtr(), &this->m_rosQueue);

            this->m_commandSub = this->m_rosNode->subscribe(command_sub);

            /*****************************************************************************************************************************/

            /***************************************************  ROS MISC ***************************************************************/

            this->m_rosQueueThread = std::thread(std::bind(&TransponderPlugin::queueThread, this));
            gzmsg << "transponder plugin loaded\n";
        }

        // currently publish noisy position to gazebo topic
        private: void sendLocation()
        {
            // randomly generate from normal distribution for noise
            std::random_device rd{};
            std::mt19937 gen{rd()};
            std::normal_distribution<> d(this->m_noiseMu, this->m_noiseSigma);

            // Gazebo publishing transponder's position with noise and delay
            auto curr_pose = this->m_model->WorldPose();
            ignition::math::Vector3<double> position = curr_pose.Pos();
            auto pub_msg = msgs::Vector3d();
            // std::cout << position.X() << " " << position.Y() << " " << position.Z() << std::endl;
            pub_msg.set_x(position.X() + d(gen));
            pub_msg.set_y(position.Y() + d(gen));
            pub_msg.set_z(position.Z() + d(gen));
            this->m_globalPosPub->Publish(pub_msg);
        }

        // gets temperature from sensor to adjust sound speed
        private: void temperatureRosCallback(const std_msgs::Float64ConstPtr &msg)
        {
            this->m_temperature = msg->data;
            auto my_pos = this->m_model->WorldPose();

            // Base on https://dosits.org/tutorials/science/tutorial-speed/
            this->m_soundSpeed = 1540.4 + my_pos.Pos().Z() / 1000 * 17 + (this->m_temperature - 10) * 4;
            gzmsg << "Detected change of temperature, transponder sound speed is now: " << this->m_soundSpeed << " m/s\n";
        }

        // receives ping from transponder and call Send()
        private: void iisRosCallback(const std_msgs::StringConstPtr &msg)
        {
            auto box = this->m_model->GetWorld()->ModelByName("box");
            double dist = (this->m_model->WorldPose().Pos() - box->WorldPose().Pos()).Length();
            std::string command = msg->data;
            if(!command.compare("ping"))
            {
                gzmsg << this->m_transponderDevice+ "_" + this->m_transponderID + ": Received iis_ping, responding\n";
                sleep(dist / this->m_soundSpeed);
                sendLocation();
            }
            else
            {
                gzmsg << "Unknown command, ignore\n";
            }

        }

        // receives ping from transponder and call Send()
        private: void cisRosCallback(const std_msgs::StringConstPtr &msg)
        {
            auto box = this->m_model->GetWorld()->ModelByName("box");
            double dist = (this->m_model->WorldPose().Pos() - box->WorldPose().Pos()).Length();
            std::string command = msg->data;
            if(!command.compare("ping"))
            {
                // gzmsg << this->m_transponderDevice+ "_" + this->m_transponderID + ": Received cis_ping, responding\n";
                sleep(dist / this->m_soundSpeed);
                sendLocation();
            }
            else
            {
                gzmsg << "Unknown command, ignore\n";
            }
        }

        private: void commandRosCallback(const usbl_gazebo::USBLCommandConstPtr& msg)
        {
            // gzmsg << "transponder ID: " << msg->commandID << ", command ID" << msg->transponderID << ", data: " << msg->data << std::endl;

            // send back command response
            usbl_gazebo::USBLResponse response_msg;
            response_msg.data = "hi from transponder_" + this->m_transponderID; // just some random message, doesn't mean anything
            response_msg.responseID = 1;
            response_msg.transceverID = this->m_transceiverID.back() - '0'; // get numerical value from char
            this->m_commandResponsePub.publish(response_msg);
        }


        private: void queueThread()
        {
            static const double timeout = 0.01;
            while (this->m_rosNode->ok())
            {
                this->m_rosQueue.callAvailable(ros::WallDuration(timeout));
            }
        }

        // transponder attributes
        private: std::string m_namespace;
        private: std::string m_transponderDevice;
        private: std::string m_transponderID;
        private: std::string m_transceiverDevice;
        private: std::string m_transceiverID;

        // environment variables
        private: double m_temperature;
        private: double m_soundSpeed;
        private: double m_noiseMu;
        private: double m_noiseSigma;

        // Gazebo nodes, publishers, and subscribers
        private: transport::NodePtr m_gzNode;
        private: transport::PublisherPtr m_globalPosPub;
        private: physics::ModelPtr m_model;

        // ROS nodes, publishers and subscibers
        private: ros::Publisher m_commandResponsePub;
        private: ros::Subscriber m_iisSub;
        private: ros::Subscriber m_cisSub;
        private: ros::Subscriber m_commandSub;
        private: ros::Subscriber m_temperatureSub;
        private: ros::CallbackQueue m_rosQueue;

        private: std::unique_ptr<ros::NodeHandle> m_rosNode;
        private: std::thread m_rosQueueThread;
    };


GZ_REGISTER_MODEL_PLUGIN(TransponderPlugin)

}