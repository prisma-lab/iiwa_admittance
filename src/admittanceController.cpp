#include "ros/ros.h"
#include "boost/thread.hpp"
#include "sensor_msgs/JointState.h"
#include "geometry_msgs/PoseStamped.h"
#include "geometry_msgs/TwistStamped.h"
#include "geometry_msgs/AccelStamped.h"
#include "gazebo_msgs/ContactsState.h"
#include <std_msgs/Float64.h>
#include <std_msgs/Float64MultiArray.h>
#include <geometry_msgs/WrenchStamped.h>
#include <geometry_msgs/PointStamped.h>

#include <kdl_parser/kdl_parser.hpp>
#include <kdl/chainfksolvervel_recursive.hpp>
#include <kdl/chainfksolverpos_recursive.hpp>
#include <kdl/chainiksolvervel_pinv.hpp>
#include <kdl/chainfksolverpos_recursive.hpp>
#include <kdl/chainiksolverpos_nr.hpp>
#include <kdl/chainjnttojacsolver.hpp>
#include <kdl/chainjnttojacdotsolver.hpp>
#include <kdl/chaindynparam.hpp>

#include "../include/kuka_control/planner.h"
#include <kuka_control/waypointsAction.h>
#include <actionlib/server/simple_action_server.h>

#include "../include/kuka_control/LowPassFilter.hpp"

using namespace std;

void rotateYaw(const geometry_msgs::PoseStamped& init, geometry_msgs::PoseStamped& final, double incyaw) {
  tf::Matrix3x3 initR;
  tf::Quaternion initq(init.pose.orientation.x,init.pose.orientation.y,init.pose.orientation.z,init.pose.orientation.w);
  initR.setRotation(initq);

  double roll,pitch,yaw;
  initR.getRPY(roll,pitch,yaw);
  yaw+=incyaw;
  initR.setRPY(roll,pitch,yaw);
  initR.getRotation(initq);

  final.pose.orientation.x = initq.x();
  final.pose.orientation.y = initq.y();
  final.pose.orientation.z = initq.z();
  final.pose.orientation.w = initq.w();
}

class ETank {
	public:
		ETank(double Einit, double Emin, double Emax, double dt) {_Et=Einit;_Emin=Emin; _Emax=Emax; _xt=sqrt(2*_Et);_dt=dt;};
		void update(const Eigen::VectorXd input, const Eigen::MatrixXd Kd, const Eigen::VectorXd x_dot, const Eigen::VectorXd Kpxtilde);
		double getEt() {return _Et;};
		double _alpha;
	private:
		double _Et, _Emin, _Emax, _xt;
		double _beta;
		double _dt;
};

void ETank::update(const Eigen::VectorXd input, const Eigen::MatrixXd Kd, const Eigen::VectorXd x_dot, const Eigen::VectorXd Kpxtilde) {
	if(_Et<=_Emax) _beta=1;
	else _beta=0;

	double w = input.dot(Kpxtilde);

	double f_energy = 0.5*( 1 - cos(M_PI*(_Et-_Emin)/(_Emax-_Emin)) );
	double g_input;
	if(w<=0) g_input=0;
	else	g_input=1;

	_alpha = f_energy*g_input + (1-g_input);

	double gamma;
	if( (_Et>=_Emin) && w>=0 ) gamma=_alpha;
	else gamma=0;

	w=gamma*w;

	double eta=0.5;
	double Diss = x_dot.transpose()*Kd*x_dot;

	double ut = -(1.0/_xt)*w;
	cout<<"Ut: "<<ut<<endl;
	double xt_dot = (_beta*eta/_xt)*Diss + ut;
	_xt += xt_dot*_dt;
	_Et = 0.5*_xt*_xt;

}

class ETankGen {
	public:
		ETankGen(double Einit, double Emin, double Emax, double dt, int inputSize);
		void update(const std::vector<double> inputs, const std::vector<double> dissInputs);
		double getEt() {return _Et;};
		std::vector<double> _alpha;
	private:
		double _Et, _Emin, _Emax, _xt;
		double _beta;
		double _dt;
};

ETankGen::ETankGen(double Einit, double Emin, double Emax, double dt, int inputSize) {
	_Et=Einit;
	_Emin=Emin;
	_Emax=Emax;
	_xt=sqrt(2*_Et);
	_dt=dt;
	for (int i=0; i<inputSize; i++) {
		_alpha.push_back(1);
	}
}

void ETankGen::update(const std::vector<double> inputs, const std::vector<double> dissInputs) {
	if(_Et<=_Emax) _beta=1;
	else _beta=0;

	double f_energy = 0.5*( 1 - cos(M_PI*(_Et-_Emin)/(_Emax-_Emin)) );
	double eta=0.8;
	double Diss=0;
	double wtot = 0;

	for (int i=0; i<dissInputs.size(); i++)
		Diss+=dissInputs[i];

	for (int i=0; i<_alpha.size(); i++) {
		double w = inputs[i];
		double g_input;
		if(w<=0) g_input=0;
		else	g_input=1;

		_alpha[i] = f_energy*g_input + (1-g_input);
		double gamma;
		if( (_Et>=_Emin) && w>=0 ) gamma=_alpha[i];
		else gamma=0;

		//if(i==1) cout<<"Power: "<<gamma*w<<endl;

		wtot+=gamma*w;
		//if(w!=0) cout<<"Power: "<<w<<"  Diss: "<<Diss<<endl;
	}

	double ut = -(1.0/_xt)*wtot;
	double xt_dot = (_beta*eta/_xt)*Diss + ut;
	_xt += xt_dot*_dt;
	if (_xt>sqrt(_Emax*2)) _xt=sqrt(_Emax*2);
	_Et = 0.5*_xt*_xt;
}


class DERIV {
	public:
		DERIV(double freq=500,double gain=100) {_f=freq;_dt=1.0/_f;_integral=Eigen::VectorXd::Zero(6);_gain=gain;};
		void update(Eigen::VectorXd x) {_xd=_gain*(x-_integral); _integral+=_gain*_dt*(x-_integral);};

		Eigen::VectorXd _xd;
	private:
		double _f,_dt;
		Eigen::VectorXd _integral;
		double _gain;

};

enum diverterState {IMPACT, DETACHED, HOOKED, NORMAL};

class KUKA_INVDYN {
	public:
		KUKA_INVDYN(double sampleTime);
		void run();
		bool init_robot_model();
		void get_dirkin();

		void joint_states_cb( sensor_msgs::JointState );
		void interaction_wrench_cb(const gazebo_msgs::ContactsStateConstPtr&);
		void real_interaction_wrench_cb(const geometry_msgs::WrenchStampedConstPtr&);
		void drone_posfb_cb(const std_msgs::Float64MultiArrayConstPtr& message);
		void ctrl_loop();
		void compute_force_errors(const Eigen::VectorXd h, const Eigen::VectorXd hdot, const Eigen::VectorXd mask);
		void compute_errors(const geometry_msgs::PoseStamped& p_des, const geometry_msgs::TwistStamped& v_des, const geometry_msgs::AccelStamped& a_des);
		void compute_compliantFrame(const geometry_msgs::PoseStamped& p_des, const geometry_msgs::TwistStamped& v_des, const geometry_msgs::AccelStamped& a_des);
		void compute_compliantFrame(const geometry_msgs::PoseStamped& p_des, const geometry_msgs::TwistStamped& v_des, const geometry_msgs::AccelStamped& a_des, const std::vector<double> alpha);
		bool newTrajectory(const std::vector<geometry_msgs::PoseStamped> waypoints, const std::vector<double> times);
		bool newTrajectory(const std::vector<geometry_msgs::PoseStamped> waypoints, const std::vector<double> times, const Eigen::VectorXd xdi, const Eigen::VectorXd xdf, const Eigen::VectorXd xddi, const Eigen::VectorXd xddf);
		bool newForceTrajectory(const std::vector<Eigen::VectorXd> waypoints, const std::vector<double> times, const Eigen::VectorXd mask);
		bool getPose(geometry_msgs::PoseStamped& p_des);
		bool getDesPose(geometry_msgs::PoseStamped& p_des);
		bool getWrench(Eigen::VectorXd& _wrench);
		bool robotReady() {return _first_fk;};
		void exitForceControl() {_fControl=false;};
		const diverterState getState() {return _state;};
		void actionCB(const kuka_control::waypointsGoalConstPtr &goal);
		void setDone(bool done) {_mainDone=done;};
	private:
		void updatePose();
		void updateForce();
		void updateState();
		ros::NodeHandle _nh;
		KDL::Tree iiwa_tree;

		KDL::ChainFkSolverPos_recursive *_fksolver; //Forward position solver
		KDL::ChainFkSolverVel_recursive *_fk_solver_pos_vel; //Forward position and velocity solver
		KDL::ChainIkSolverVel_pinv *_ik_solver_vel;   	//Inverse velocity solver
		KDL::ChainIkSolverPos_NR *_ik_solver_pos;
		KDL::ChainJntToJacSolver *_J_solver;
		KDL::ChainJntToJacDotSolver *_Jdot_solver;

		KDL::Chain _k_chain;

		ros::Subscriber _js_sub;
		ros::Publisher _js_pub;
		ros::Subscriber _wrench_sub, _real_wrench_sub, _dronePosFb_sub;
		ros::Publisher _cartpose_pub, _cartvel_pub, _desPose_pub, _extWrench_pub, _linearDifference_pub, _linearVelDifference_pub;
		ros::Publisher _plannedpose_pub,_plannedtwist_pub,_plannedacc_pub,_plannedwrench_pub;
		ros::Publisher _robotEnergy_pub, _totalEnergy_pub, _tankEnergy_pub, _totalPower_pub, _kpvalue_pub, _kdvalue_pub;
		KDL::JntArray *_initial_q;
		KDL::JntArray *_q_in;
		KDL::JntArray *_q_out;
		KDL::JntArray *_q_in_old;
		KDL::JntArray *_dq_in;
		ros::Publisher _cmd_pub[7];
		bool _first_js;
		bool _first_fk;
		bool _sync, _first_wrench;
		KDL::FrameVel _dirkin_out;
		KDL::Frame _p_out;
		KDL::Twist _v_out;
		KDL::ChainDynParam *_dyn_param;
		geometry_msgs::PoseStamped _pose;
		geometry_msgs::TwistStamped _vel;
		Eigen::VectorXd _acc;
		Eigen::VectorXd x_t;
		Eigen::VectorXd xDot_t;
		Eigen::VectorXd xDotDot;
		Eigen::MatrixXd _J;
		Eigen::MatrixXd _Jold;
		Eigen::MatrixXd _JDot;
		Eigen::VectorXd _gradManMeas;
		Eigen::VectorXd _extWrench, _wrenchBias;
		int _wrenchCount;
		Eigen::VectorXd z_t,zDot_t,zDotDot_t;
		geometry_msgs::PoseStamped _complPose;
		geometry_msgs::TwistStamped _complVel;
		geometry_msgs::AccelStamped _complAcc;
		geometry_msgs::PoseStamped _desPose;
		geometry_msgs::TwistStamped _desVel;
		geometry_msgs::AccelStamped _desAcc;
		bool _fControl;
		bool _trajEnd;
		bool _newPosReady;
		geometry_msgs::PoseStamped _nextdesPose;
		geometry_msgs::TwistStamped _nextdesVel;
		geometry_msgs::AccelStamped _nextdesAcc;
		Eigen::MatrixXd _Mt;
		Eigen::MatrixXd _Kdt;
		Eigen::MatrixXd _Kpt;
		Eigen::VectorXd xf,xf_dot,xf_dotdot;
		Eigen::VectorXd _h_des,_hdot_des, _nexth_des,_nexthdot_des, _forceMask;
		DERIV numericAcc;
		double _sTime,_freq;
		actionlib::SimpleActionServer<kuka_control::waypointsAction> _kukaActionServer;
		kuka_control::waypointsFeedback _actionFeedback;
  		kuka_control::waypointsResult _actionResult;
		LowPassFilter lpf[6];
		double _admittanceEnergy, _forcesEnergy, _contTime;
		diverterState _state;
		bool _firstCompliant, _mainDone, _dronePos_ready;
		Eigen::Vector3d _dronePos;
};

bool KUKA_INVDYN::init_robot_model() {
	/*
	std::string robot_desc_string;
	_nh.param("robot_description", robot_desc_string, std::string());
	if (!kdl_parser::treeFromString(robot_desc_string, iiwa_tree)){
		ROS_ERROR("Failed to construct kdl tree");
		return false;
	}
	else {
		ROS_INFO("Robot tree found!");
	}
	*/

    if (!kdl_parser::treeFromFile("/home/jcacace/dev/ros_ws/src/IIWA/iiwa_admittance/urdf/iiwa7.urdf", iiwa_tree)){
    	ROS_ERROR("Failed to construct kdl tree");
      	return false;
    }

	std::string base_link = "iiwa_link_0";
	std::string tip_link  = "iiwa_link_sensor_kuka";
	//std::string base_link = "world";
	//std::string tip_link  = "lbr_iiwa_link_7";
	if ( !iiwa_tree.getChain(base_link, tip_link, _k_chain) ) return false;

	_fksolver = new KDL::ChainFkSolverPos_recursive( _k_chain );
	_fk_solver_pos_vel = new KDL::ChainFkSolverVel_recursive( _k_chain );
	_ik_solver_vel = new KDL::ChainIkSolverVel_pinv( _k_chain );
	_ik_solver_pos = new KDL::ChainIkSolverPos_NR( _k_chain, *_fksolver, *_ik_solver_vel, 500, 1e-6 );
	_J_solver = new KDL::ChainJntToJacSolver( _k_chain );
	_Jdot_solver = new KDL::ChainJntToJacDotSolver( _k_chain );
	//_Jdot_solver->setRepresentation(2);//INERTIAL
	//_Jdot_solver->setRepresentation(0);//HYBRID

	_q_in = new KDL::JntArray( _k_chain.getNrOfJoints() );
	_q_out = new KDL::JntArray( _k_chain.getNrOfJoints() );
	_q_in_old = new KDL::JntArray( _k_chain.getNrOfJoints() );
	_dq_in = new KDL::JntArray( _k_chain.getNrOfJoints() );
	_initial_q = new KDL::JntArray( _k_chain.getNrOfJoints() );
	_dyn_param = new KDL::ChainDynParam(_k_chain,KDL::Vector(0,0,-9.81));

	return true;
}

KUKA_INVDYN::KUKA_INVDYN(double sampleTime) :
    _kukaActionServer(_nh, "kukaActionServer", boost::bind(&KUKA_INVDYN::actionCB, this, _1), false) {

	_sTime=sampleTime;
	_freq = 1.0/_sTime;

	if (!init_robot_model()) exit(1);
	ROS_INFO("Robot tree correctly loaded from parameter server!");

	cout << "Joints and segments: " << iiwa_tree.getNrOfJoints() << " - " << iiwa_tree.getNrOfSegments() << endl;


	_js_sub = _nh.subscribe("/iiwa/joint_states", 0, &KUKA_INVDYN::joint_states_cb, this);
	_js_pub = _nh.advertise<std_msgs::Float64MultiArray>("/iiwa/jointsCommand", 0);
	_wrench_sub = _nh.subscribe("/tool_contact_sensor_state", 0, &KUKA_INVDYN::interaction_wrench_cb, this);
	_real_wrench_sub = _nh.subscribe("/netft_data", 0, &KUKA_INVDYN::real_interaction_wrench_cb, this);
	_dronePosFb_sub = _nh.subscribe("/controller/posFeedback", 0, &KUKA_INVDYN::drone_posfb_cb, this);

	_cartpose_pub = _nh.advertise<geometry_msgs::PoseStamped>("/iiwa/eef_pose", 0);
	_cartvel_pub = _nh.advertise<geometry_msgs::TwistStamped>("/iiwa/eef_twist", 0);
	_plannedpose_pub = _nh.advertise<geometry_msgs::PoseStamped>("/iiwa/cmd/pose", 0);
	_plannedtwist_pub = _nh.advertise<geometry_msgs::TwistStamped>("/iiwa/planned_twist", 0);
	_plannedacc_pub = _nh.advertise<geometry_msgs::AccelStamped>("/iiwa/planned_acc", 0);
	_plannedwrench_pub = _nh.advertise<std_msgs::Float64>("/iiwa/planned_wrench", 0);
	_desPose_pub = _nh.advertise<geometry_msgs::PoseStamped>("/iiwa/eef_des_pose", 0);
	_extWrench_pub = _nh.advertise<geometry_msgs::WrenchStamped>("/iiwa/eef_ext_wrench", 0);
	_robotEnergy_pub = _nh.advertise<std_msgs::Float64>("/iiwa/admittance_energy", 0);
	_totalEnergy_pub = _nh.advertise<std_msgs::Float64>("/iiwa/total_energy", 0);
	_totalPower_pub = _nh.advertise<std_msgs::Float64>("/iiwa/total_power", 0);
	_tankEnergy_pub = _nh.advertise<std_msgs::Float64>("/iiwa/tank_energy", 0);
	_linearDifference_pub = _nh.advertise<geometry_msgs::PointStamped>("/iiwa/linearDifference", 0);
	_linearVelDifference_pub = _nh.advertise<geometry_msgs::PointStamped>("/iiwa/linearVelDifference", 0);
	_kpvalue_pub = _nh.advertise<std_msgs::Float64MultiArray>("/iiwa/admit_gains", 0);

	//_cmd_pub[0] = _nh.advertise< std_msgs::Float64 > ("iiwa/joint1_position_controller/command", 0);
	//_cmd_pub[1] = _nh.advertise< std_msgs::Float64 > ("iiwa/joint2_position_controller/command", 0);
	//_cmd_pub[2] = _nh.advertise< std_msgs::Float64 > ("iiwa/joint3_position_controller/command", 0);
	//_cmd_pub[3] = _nh.advertise< std_msgs::Float64 > ("iiwa/joint4_position_controller/command", 0);
	//_cmd_pub[4] = _nh.advertise< std_msgs::Float64 > ("iiwa/joint5_position_controller/command", 0);
	//_cmd_pub[5] = _nh.advertise< std_msgs::Float64 > ("iiwa/joint6_position_controller/command", 0);
	//_cmd_pub[6] = _nh.advertise< std_msgs::Float64 > ("iiwa/joint7_position_controller/command", 0);

	x_t.resize(6);
	xDot_t.resize(6);
	xDotDot.resize(6);
	_extWrench.resize(6);
	_extWrench = Eigen::VectorXd::Zero(6);
	_J.resize(6,_k_chain.getNrOfJoints());
	_Jold.resize(6,_k_chain.getNrOfJoints());
	_JDot.resize(6,_k_chain.getNrOfJoints());
	_J = MatrixXd::Zero(6,_k_chain.getNrOfJoints());
	_Jold = MatrixXd::Zero(6,_k_chain.getNrOfJoints());
	_gradManMeas.resize(7);
	_gradManMeas = Eigen::VectorXd::Zero(7);

	z_t.resize(6); z_t=Eigen::VectorXd::Zero(6);
	zDot_t.resize(6); zDot_t=Eigen::VectorXd::Zero(6);
	zDotDot_t.resize(6); zDotDot_t=Eigen::VectorXd::Zero(6);

	xf.resize(7); xf=Eigen::VectorXd::Zero(7);
	xf_dot.resize(6); xf_dot=Eigen::VectorXd::Zero(6);
	xf_dotdot.resize(6); xf_dotdot=Eigen::VectorXd::Zero(6);

	_Mt =  1*Eigen::MatrixXd::Identity(6,6); //1
	_Kdt = 15*Eigen::MatrixXd::Identity(6,6); //15
	_Kpt = 13*Eigen::MatrixXd::Identity(6,6); //10

	//_Mt.bottomRightCorner(3,3) = 70*Eigen::MatrixXd::Identity(3,3);
	//_Kpt.bottomRightCorner(3,3) = 1000*Eigen::MatrixXd::Identity(3,3);
	//_Mt(1,1) = 3;
	//_Kdt(1,1) = 150;
	//_Kpt(1,1) = 30; 

	_wrenchCount = 0;
	_wrenchBias = Eigen::VectorXd::Zero(6);


	_h_des.resize(6); _h_des=Eigen::VectorXd::Zero(6);
	_hdot_des.resize(6); _hdot_des=Eigen::VectorXd::Zero(6);
	_acc.resize(6);_acc=Eigen::VectorXd::Zero(6);
	_dronePos = Eigen::Vector3d::Zero();

	_admittanceEnergy = 0;
	_state = NORMAL;

	_first_js = false;
	_first_fk = false;
	_fControl = false;
	_trajEnd = true;
	_newPosReady = false;
	_first_wrench = false;
	_firstCompliant = false;
	_mainDone = false;

	_contTime=0;

	_kukaActionServer.start();
}

void KUKA_INVDYN::drone_posfb_cb(const std_msgs::Float64MultiArrayConstPtr& message) {
	_dronePos(0) = message->data[0];
	_dronePos(1) = message->data[1];
	_dronePos(2) = message->data[2];

	_dronePos_ready = true;
}

bool KUKA_INVDYN::getPose(geometry_msgs::PoseStamped& p_des) {
	if(!_first_fk) return false;

	p_des = _pose;
	return true;
}

bool KUKA_INVDYN::getDesPose(geometry_msgs::PoseStamped& p_des) {
	if(!_first_fk) return false;

	p_des = _desPose;
	return true;
}

bool KUKA_INVDYN::getWrench(Eigen::VectorXd& wrench) {
	if(!_first_fk) return false;

	wrench = _extWrench;
	return true;
}

void KUKA_INVDYN::interaction_wrench_cb(const gazebo_msgs::ContactsStateConstPtr& message) {

	int nContacts=message->states.size();

	if(nContacts==0) {
		_extWrench = Eigen::VectorXd::Zero(6);
	}
	else {
		for (int i=0; i<nContacts; i++) {
			_extWrench(0)=message->states[i].total_wrench.force.x;
			_extWrench(1)=message->states[i].total_wrench.force.y;
			_extWrench(2)=message->states[i].total_wrench.force.z;
			_extWrench(3)=message->states[i].total_wrench.torque.x;
			_extWrench(4)=message->states[i].total_wrench.torque.y;
			_extWrench(5)=message->states[i].total_wrench.torque.z;
		}
		tf::Quaternion qe(_pose.pose.orientation.x,_pose.pose.orientation.y,_pose.pose.orientation.z,_pose.pose.orientation.w);
		tf::Matrix3x3 Re_tf;
		Eigen::Matrix3d Re;
		Re_tf.setRotation(qe);
		tf::matrixTFToEigen(Re_tf,Re);
		_extWrench.head(3) = Re*_extWrench.head(3);
		_extWrench.tail(3) = Re*_extWrench.tail(3);
		geometry_msgs::WrenchStamped wrenchstamp;
		wrenchstamp.header.stamp = ros::Time::now();
		wrenchstamp.wrench.force.x = _extWrench(0);
		wrenchstamp.wrench.force.y = _extWrench(1);
		wrenchstamp.wrench.force.z = _extWrench(2);
		wrenchstamp.wrench.torque.x = _extWrench(3);
		wrenchstamp.wrench.torque.y = _extWrench(4);
		wrenchstamp.wrench.torque.z = _extWrench(5);
		_extWrench_pub.publish(wrenchstamp);
		//cout<<_extWrench<<endl<<endl;
	}

	_first_wrench=true;

}

void KUKA_INVDYN::real_interaction_wrench_cb(const geometry_msgs::WrenchStampedConstPtr& message) {
	
	Eigen::VectorXd localWrench, outWrench;
	int nSamples = 500;
	localWrench.resize(6);
	outWrench.resize(6);

	localWrench(0)=message->wrench.force.x;
	localWrench(1)=message->wrench.force.y;
	localWrench(2)=message->wrench.force.z;
	localWrench(3)=message->wrench.torque.x;
	localWrench(4)=message->wrench.torque.y;
	localWrench(5)=message->wrench.torque.z;

	if(_wrenchCount<nSamples) {
		_wrenchBias += localWrench;
		_wrenchCount++;
		return;
	}
	else if (_wrenchCount==nSamples) {
		_wrenchBias/=_wrenchCount;
		_wrenchCount++;
		ROS_WARN("Force biased!");
		cout<<_wrenchBias.transpose()<<endl;
	}
	
	for(int i=0; i<6; i++)
		localWrench(i) -= _wrenchBias(i);

	outWrench = localWrench;

	for (int i=0; i<6; i++) {
		//cout<<lpf[i].update(localWrench(i),0.002,100.0/(2.0*M_PI))<<"  real:"<<localWrench(i)<<endl;
		localWrench(i) = lpf[i].update(localWrench(i),0.002,5.0/(2.0*M_PI));
	}
	//cout<<endl;

	tf::Quaternion qe(_pose.pose.orientation.x,_pose.pose.orientation.y,_pose.pose.orientation.z,_pose.pose.orientation.w);
    Vector3d pe(_pose.pose.position.x,_pose.pose.position.y,_pose.pose.position.z);
    tf::Matrix3x3 Re_tf;
    Eigen::Matrix3d Re;
    Re_tf.setRotation(qe);
    tf::matrixTFToEigen(Re_tf,Re);
    MatrixXd staticTransf(6,6);
    staticTransf << Re, MatrixXd::Zero(3,3),
                    Skew(pe)*Re, Re;
    localWrench = staticTransf*localWrench;
    outWrench = staticTransf*outWrench;
	outWrench = localWrench;
	geometry_msgs::WrenchStamped wrenchstamp;
	wrenchstamp.header.stamp = ros::Time::now();
	wrenchstamp.wrench.force.x = outWrench(0);
	wrenchstamp.wrench.force.y = outWrench(1);
	wrenchstamp.wrench.force.z = outWrench(2);
	wrenchstamp.wrench.torque.x = outWrench(3);
	wrenchstamp.wrench.torque.y = outWrench(4);
	wrenchstamp.wrench.torque.z = outWrench(5);

	_extWrench = localWrench;
	
	//Eigen::Vector3d S(0.0,0.0,0.1); //braccio
	//_extWrench.tail(3) = Skew(S) * _extWrench.head(3) + _extWrench.tail(3);

	if(outWrench.norm()<1000)
		_extWrench_pub.publish(wrenchstamp);

	if((_extWrench.norm()<1000) && _firstCompliant && (zDot_t.norm()!=0)) {
		//cout<<"calcolo"<<endl;	
		//_forcesEnergy += zDot_t.head(3).dot(_extWrench.head(3)) * (1.0/500.0);
		//double totalPower = zDot_t.dot(_extWrench);
	}

	_first_wrench=true;
}

void KUKA_INVDYN::joint_states_cb( sensor_msgs::JointState js ) {
	//cout<<"Joint states"<<endl;
	_q_in_old->data=_q_in->data;

	for(int i=0; i<7; i++ ) {
		_q_in->data[i] = js.position[i];
		_dq_in->data[i] = js.velocity[i];
		if( !_first_js ) {
			_initial_q->data[i] = js.position[i];
			_q_out->data[i] = js.position[i];
		}
	}

	get_dirkin();

	if(_first_js) {
		Eigen::MatrixXd man = _J*_J.transpose();
		double manMeas = sqrt(man.determinant());
		//cout<<manMeas<<endl<<endl;
		for(int i=0; i<7; i++)
			_gradManMeas(i) = manMeas/(_q_in->data[i] - _q_in_old->data[i]);
	}

	_first_js = true;
	_sync = true;
}

void KUKA_INVDYN::updateState() {
	//return;
	double uptresh = 0.5, imptresh = 3, timetresh = 1.0;
	bool up_cond = (_extWrench.norm()>uptresh) && (_extWrench.norm()<imptresh);
	//bool down_cond = _extWrench.norm()<lowtresh;

	//double uptresh = 0.05, lowtresh = 0.03, imptresh = 0.2, timetresh = 1.0;
	//double lowtresh = 0.02;
	//bool up_cond = (zDotDot_t.norm()>uptresh) && (zDotDot_t.norm()<imptresh);
	bool down_cond = ( _extWrench(2)>(-2) ) && ( _extWrench(2)<(-1) );

	switch(_state) {
        case NORMAL:
          if( up_cond ) {
			  _contTime += 1.0/_freq;
			  ROS_WARN("Entrato");
			  if(_contTime>(timetresh*2)) {
				_state = HOOKED;
				ROS_WARN("STATE: HOOKED!");
				_contTime = 0;
			  }
		  } else
		  {
			  _contTime = 0;
		  }
          break;
        case HOOKED:
          if( down_cond ) {
			  _contTime += 1.0/_freq;
			  ROS_WARN("Uscendo");
			  if(_contTime>(timetresh*4)) {
				//_state = NORMAL;
				//ROS_WARN("STATE: NORMAL!");
				_state = DETACHED;
				ROS_WARN("STATE: DETACHED!");
				_contTime = 0;
			  }
          } else {
			  _contTime = 0;
		  }
          break;
        case DETACHED:
			if( _trajEnd ) {
			  _contTime += 1.0/_freq;
			  if(_contTime>(timetresh*4)) {
				_state = IMPACT;
				ROS_WARN("STATE: IMPACT!");
				_contTime = 0;
			  }
          	} else {
				_contTime = 0;
		  	}
        	break;
		case IMPACT:
			if( _mainDone ) {
			  _contTime += 1.0/_freq;
			  if(_contTime>(timetresh*4)) {
				_state = NORMAL;
				ROS_WARN("STATE: NORMAL!");
				_contTime = 0;
			  }
          	} else {
				_contTime = 0;
		  	}
        	break;
      }
}

void KUKA_INVDYN::ctrl_loop() {

	std_msgs::Float64 cmd[7];
	std_msgs::Float64MultiArray jcmd;
	jcmd.data.resize(7);
  KDL::JntArray coriol_(7);
  KDL::JntArray grav_(7);
	KDL::JntArray q_out_new(_k_chain.getNrOfJoints());
	
	KDL::JntArray qd_out(_k_chain.getNrOfJoints());

  KDL::JntSpaceInertiaMatrix jsim_;
  jsim_.resize(_k_chain.getNrOfJoints());

	ros::Rate r(_freq);

	//ETank tank(1.0,0.01,1.0,_sTime);
	//ETankGen tankGen(3.0,0.01,3.0,_sTime,1);
	ETankGen stiffnessTank(0.5,0.01,0.5,_sTime,1);

	Eigen::MatrixXd finalKp = 1*_Kpt;
	Eigen::MatrixXd initialKp = _Kpt;
	Eigen::MatrixXd KpDot = Eigen::MatrixXd::Zero(6,6);
	Eigen::MatrixXd finalKd = 3*_Kdt; //4
	Eigen::MatrixXd initialKd = _Kdt;
	Eigen::MatrixXd KdDot = Eigen::MatrixXd::Zero(6,6);
	Eigen::MatrixXd finalM = 3*_Mt; //4
	Eigen::MatrixXd initialM = _Mt;
	Eigen::MatrixXd MDot = Eigen::MatrixXd::Zero(6,6);
	double finalT = 0.5; //transition in 0.5 seconds

	bool emergencyShut = false;
	LowPassFilter* dronepos_filter[3];
	for (int i=0; i<3; i++)
		dronepos_filter[i] = new LowPassFilter(30.0/(2.0*M_PI),(1.0/_freq));

	while( !_first_js ) usleep(0.1);
	while( !_first_wrench ) usleep(0.1);

	while( ros::ok() && (!emergencyShut)) {

    while( !_sync ) usleep(0.1);

	/*	if(_fControl) {
			updateForce();
			compute_force_errors(_h_des, _hdot_des,_forceMask);
			_newPosReady=true;
		}
		*/
		updatePose();

/*
		Eigen::VectorXd desVelEigen, desAccEigen, complVelEigen;
		twist2Vector(_desVel,desVelEigen);
		accel2Vector(_desAcc,desAccEigen);
		twist2Vector(_complVel,complVelEigen);
		std::vector<Eigen::VectorXd> tankInputs, tankProds;
		std::vector<double> tankDiss;
		tankDiss.push_back(complVelEigen.transpose()*_Kdt*complVelEigen);
		tankInputs.push_back(-desVelEigen);
		tankProds.push_back(_Kpt*z_t);
		tankInputs.push_back(_Mt*desAccEigen + _Kdt*desVelEigen);
		tankProds.push_back(complVelEigen);
		tankGen.update(tankInputs,tankDiss,tankProds);
		*/
		//cout<<tankGen.getEt()<<endl;

		//compute_compliantFrame(_desPose,_desVel,_desAcc,tankGen._alpha);

		if( (_state == HOOKED) || (_state == IMPACT) ) {
			if(_state == HOOKED) {
				finalKp = 13*Eigen::MatrixXd::Identity(6,6); 
				finalKd = 2*15*Eigen::MatrixXd::Identity(6,6); //4
				finalM =  3*1*Eigen::MatrixXd::Identity(6,6); //3
			}
			else if(_state == IMPACT) {
				finalKp = 400*Eigen::MatrixXd::Identity(6,6); 
				finalKd = 800*Eigen::MatrixXd::Identity(6,6);
				finalM =  30*Eigen::MatrixXd::Identity(6,6);
				finalKp(1,1) = 20;
				finalKd(1,1) = 30;
				finalM(1,1) = 1;
			}

			for(int i=0; i<6; i++) {
				if(_Kpt(i,i)<finalKp(i,i)) {
					//ROS_WARN("POSITIVA");
					KpDot(i,i) = ((finalKp(i,i)-initialKp(i,i))/finalT);
					//_Kpt(i,i) += (finalKp(i,i)/finalT) * _sTime;
				}
				else
					KpDot(i,i) = 0;

				if(_Kdt(i,i)<finalKd(i,i)) {
					//ROS_WARN("POSITIVA");
					KdDot(i,i) = ((finalKd(i,i)-initialKd(i,i))/finalT);
					//_Kpt(i,i) += (finalKp(i,i)/finalT) * _sTime;
				}
				else
					KdDot(i,i) = 0;

				if(_Mt(i,i)<finalM(i,i)) {
					//ROS_WARN("POSITIVA");
					MDot(i,i) = ((finalM(i,i)-initialM(i,i))/finalT);
					//_Kpt(i,i) += (finalKp(i,i)/finalT) * _sTime;
				}
				else
					MDot(i,i) = 0;
			}
		}
		else if( _state == NORMAL || (_state == DETACHED)) {
			for(int i=0; i<6; i++) {
				if(_Kpt(i,i)>initialKp(i,i)) {
					//ROS_WARN("NEGATIVA");
					//cout<<KpDot(i,i)<< " ";
					KpDot(i,i) = -((finalKp(i,i)-initialKp(i,i))/finalT);
					//_Kpt(i,i) -= (finalKp(i,i)/finalT) * _sTime;
				}
				else
					KpDot(i,i) = 0;

				if(_Kdt(i,i)>initialKd(i,i)) {
					//ROS_WARN("POSITIVA");
					KdDot(i,i) = -((finalKd(i,i)-initialKd(i,i))/finalT);
					//_Kpt(i,i) += (finalKp(i,i)/finalT) * _sTime;
				}
				else
					KdDot(i,i) = 0;

				if(_Mt(i,i)>initialM(i,i)) {
					//ROS_WARN("POSITIVA");
					MDot(i,i) = -((finalM(i,i)-initialM(i,i))/finalT);
					//_Kpt(i,i) += (finalKp(i,i)/finalT) * _sTime;
				}
				else
					MDot(i,i) = 0;
			}
		}

		std::vector<double> tankInputs;
		std::vector<double> tankDiss;

		tankDiss.push_back(zDot_t.dot(_Kdt*zDot_t));
		//Eigen::VectorXd prod = KpDot*z_t;
		//for(int i=0; i<6; i++) {
		//	tankInputs.push_back(z_t(i));
		//	tankProds.push_back(prod(i));
		//}
		if (stiffnessTank._alpha[0] != 1)
			cout<<stiffnessTank._alpha[0] << " " << _Kpt(0,0)<<endl;
		KpDot *= stiffnessTank._alpha[0];
		MDot *= stiffnessTank._alpha[0];
		tankInputs.push_back(0.5*z_t.dot(KpDot*z_t) + 0.5*zDot_t.dot(MDot*zDot_t));
		stiffnessTank.update(tankInputs,tankDiss);
	
		_Kpt += _sTime * KpDot;	
		_Mt += _sTime * MDot;
		_Kdt += _sTime * KdDot;
		if(_Kpt.norm()>finalKp.norm())
			_Kpt = finalKp;
		else if(_Kpt.norm()<initialKp.norm())
			_Kpt = initialKp;

		if(_Kdt.norm()>finalKd.norm())
			_Kdt = finalKd;
		else if(_Kdt.norm()<initialKd.norm())
			_Kdt = initialKd;

		if(_Mt.norm()>finalM.norm())
			_Mt = finalM;
		else if(_Mt.norm()<initialM.norm())
			_Mt = initialM;

		//cout<<_Mt(0,0)<<endl;
		std_msgs::Float64MultiArray msg;
		msg.data.resize(3);
		msg.data[0] = _Kpt(0,0);
		msg.data[1] = _Kdt(0,0);
		msg.data[2] = _Mt(0,0);
		_kpvalue_pub.publish(msg);

		double totalEnergy = _admittanceEnergy - _forcesEnergy + stiffnessTank.getEt();
		std_msgs::Float64 msgenergy;
		msgenergy.data = totalEnergy;
		_totalEnergy_pub.publish(msgenergy);
		//cout<<KdDot(1,1)<<endl;
		std_msgs::Float64 msgtank;
		msgtank.data = stiffnessTank.getEt();
		_tankEnergy_pub.publish(msgtank);

		_desPose_pub.publish(_desPose);
		updateState();
		compute_compliantFrame(_desPose,_desVel,_desAcc);
		//compute_errors(_complPose,_complVel,_complAcc); //Calcolo errori spazio operativo

		_complPose.header.stamp = ros::Time::now();
		_complVel.header.stamp = _complPose.header.stamp;
		_complAcc.header.stamp = _complPose.header.stamp;
		_plannedpose_pub.publish(_complPose);
		_plannedtwist_pub.publish(_complVel);
		_plannedacc_pub.publish(_complAcc);

		//printf("DesPose: x: %f - y: %f - z: %f\n", _desPose.pose.position.x,_desPose.pose.position.y,_desPose.pose.position.z);
		//printf("ComplPose: x: %f - y: %f - z: %f\n", _complPose.pose.position.x,_complPose.pose.position.y,_complPose.pose.position.z);

		KDL::Frame F_dest;
		tf::Quaternion qdes(_complPose.pose.orientation.x,_complPose.pose.orientation.y,_complPose.pose.orientation.z,_complPose.pose.orientation.w);
		tf::Matrix3x3 R(qdes);
		F_dest.M.data[0] = R[0][0];
		F_dest.M.data[1] = R[0][1];
		F_dest.M.data[2] = R[0][2];
		F_dest.M.data[3] = R[1][0];
		F_dest.M.data[4] = R[1][1];
		F_dest.M.data[5] = R[1][2];
		F_dest.M.data[6] = R[2][0];
		F_dest.M.data[7] = R[2][1];
		F_dest.M.data[8] = R[2][2];

		if(!_dronePos_ready) {
			F_dest.p.data[0] = _complPose.pose.position.x;
			F_dest.p.data[1] = _complPose.pose.position.y;
			F_dest.p.data[2] = _complPose.pose.position.z;
		} else {
			Vector3d diff = _dronePos;
			Vector3d actualPos(_pose.pose.position.x,_pose.pose.position.y,_pose.pose.position.z);
			Vector3d actualVel;
			double tresh = 0.02;//1cm
			for(int i=0; i<3; i++) {
				if(diff(i)>tresh) diff(i) = tresh;
				else if(diff(i)<(-tresh)) diff(i) = (-tresh);
			}

			actualVel(0) =_pose.pose.position.x-_complPose.pose.position.x-diff(0);
			actualVel(1) =_pose.pose.position.y-_complPose.pose.position.y-diff(1);
			actualVel(2) =_pose.pose.position.z-_complPose.pose.position.z-diff(2);
			actualVel = _freq*actualVel;
			//if (actualVel.norm()>2.0) {
			//	ROS_ERROR("Emergency shutdown!");
			//	emergencyShut=true;
			//}

			//cout<<"DronePos: "<<diff.transpose()<<endl;
			//cout<<"Vel: "<<actualVel.norm()<<endl;
			
			F_dest.p.data[0] = _complPose.pose.position.x + (dronepos_filter[0]->update(diff(0)) );
			F_dest.p.data[1] = _complPose.pose.position.y + (dronepos_filter[1]->update(diff(1)) );
			F_dest.p.data[2] = _complPose.pose.position.z + (dronepos_filter[2]->update(diff(2)) );
			//for(int i=0; i<3; i++)
			//	cout<<"real: "<<diff(i)<<" filter: "<<dronepos_filter[i]->getOutput()<<endl;
			
		}

		if(F_dest.p.data[1]>0.75) F_dest.p.data[1]=0.75; //workspace saturation


		//cout<<"joints: ";
		//for(int i=0; i<7; i++) cout<<_q_out->data[i]<<" ";
		//cout<<endl;

		if( _ik_solver_pos->CartToJnt(*_q_out, F_dest, q_out_new) != KDL::SolverI::E_NOERROR )
			cout << "failing in ik!" << endl;
		else {
			_q_out->data = q_out_new.data;
/*
			cout << "First itr" << endl;
			for(int i=0; i<7; i++) cout<<q_out_new.data[i]<<" ";
			cout << endl;

			exit(0);
*/
		}
		
		if(!emergencyShut) {
			for(int i=0; i<7; i++) jcmd.data[i]=_q_out->data[i];
			_js_pub.publish(jcmd);
		}

		//for(int i=0; i<7; i++ ) {
		//	cmd[i].data = _q_out->data[i];
		//}
		//for(int i=0; i<7; i++ ) {
		//	_cmd_pub[i].publish( cmd[i] );
		//}


		_sync = false;

		r.sleep();
	}

}

void KUKA_INVDYN::get_dirkin() {
	KDL::JntArrayVel q_qdot(*_q_in,*_dq_in);
	_fk_solver_pos_vel->JntToCart(q_qdot, _dirkin_out);
	_p_out = _dirkin_out.GetFrame();
	_v_out = _dirkin_out.GetTwist();
	_pose.pose.position.x = _p_out.p.x();
	_pose.pose.position.y = _p_out.p.y();
	_pose.pose.position.z = _p_out.p.z();

	

	double qx, qy, qz, qw;
	_p_out.M.GetQuaternion( qx, qy, qz, qw);
	//tf::Quaternion quat(qx, qy, qz, qw);
	//quat.normalize()
	_pose.pose.orientation.w = qw;
	_pose.pose.orientation.x = qx;
	_pose.pose.orientation.y = qy;
	_pose.pose.orientation.z = qz;

	if(!_first_fk) _desPose = _pose;

	_vel.twist.linear.x = _v_out.vel.x();
	_vel.twist.linear.y = _v_out.vel.y();
	_vel.twist.linear.z = _v_out.vel.z();
	_vel.twist.angular.x = _v_out.rot.x();
	_vel.twist.angular.y = _v_out.rot.y();
	_vel.twist.angular.z = _v_out.rot.z();

	KDL::Jacobian Jac(_k_chain.getNrOfJoints());
	KDL::Jacobian JacDot(_k_chain.getNrOfJoints());
	if( _J_solver->JntToJac(*_q_in, Jac) != KDL::ChainJntToJacSolver::E_NOERROR )
		cout << "failing in Jacobian computation!" << endl;

	_Jold = _J;
	_J = Jac.data;
	if( _Jdot_solver->JntToJacDot(q_qdot, JacDot) != KDL::ChainJntToJacDotSolver::E_NOERROR )
		cout << "failing in JacobianDot computation!" << endl;

	_JDot = JacDot.data;
/*	for(int i=0; i<6;i++)
		for(int j=0; j<7; j++) {
			_JDot(i,j) = (_J(i,j)-_Jold(i,j))*_freq;
		} */

	Eigen::VectorXd vel(6);
	vel = _J*(_dq_in->data);
	numericAcc.update(vel);
	_acc = numericAcc._xd;

	_vel.twist.linear.x = vel(0);
	_vel.twist.linear.y = vel(1);
	_vel.twist.linear.z = vel(2);
	_vel.twist.angular.x = vel(3);
	_vel.twist.angular.y = vel(4);
	_vel.twist.angular.z = vel(5);

	_pose.header.stamp = ros::Time::now();
	_vel.header.stamp = _pose.header.stamp;
	_cartpose_pub.publish( _pose );
	_cartvel_pub.publish( _vel );
	_first_fk = true;
}

void KUKA_INVDYN::compute_errors(const geometry_msgs::PoseStamped& p_des, const geometry_msgs::TwistStamped& v_des, const geometry_msgs::AccelStamped& a_des) {
	x_t(0) = _pose.pose.position.x - p_des.pose.position.x;
	x_t(1) = _pose.pose.position.y - p_des.pose.position.y;
	x_t(2) = _pose.pose.position.z - p_des.pose.position.z;

	x_t = -1*x_t; //inverti segno

	tf::Quaternion qe(_pose.pose.orientation.x,_pose.pose.orientation.y,_pose.pose.orientation.z,_pose.pose.orientation.w);
	tf::Quaternion qd(p_des.pose.orientation.x,p_des.pose.orientation.y,p_des.pose.orientation.z,p_des.pose.orientation.w);
	tf::Matrix3x3 Re_tf, Rd_tf;
	Eigen::Matrix3d Re,Rd;
	Re_tf.setRotation(qe);
	Rd_tf.setRotation(qd);
	tf::matrixTFToEigen(Re_tf,Re);
	tf::matrixTFToEigen(Rd_tf,Rd);

	Eigen::Matrix3d Rerr = Re.transpose()*Rd;
	tf::Matrix3x3 Rerr_tf;
	tf::matrixEigenToTF(Rerr,Rerr_tf);
	tf::Quaternion qerr;
	Rerr_tf.getRotation(qerr);
	double angle = qerr.getAngle();
	tf::Vector3 axis = qerr.getAxis();
	Eigen::Vector3d eps;
	tf::vectorTFToEigen(axis,eps);
	eps = Re*(sin(angle/2.0)*eps);
	x_t(3) = eps(0);x_t(4) = eps(1);x_t(5) = eps(2);

	//cout<<x_t<<endl<<endl;

	xDot_t(0) = _vel.twist.linear.x - v_des.twist.linear.x;
	xDot_t(1) = _vel.twist.linear.y - v_des.twist.linear.y;
	xDot_t(2) = _vel.twist.linear.z - v_des.twist.linear.z;
	xDot_t(3) = _vel.twist.angular.x - v_des.twist.angular.x;
	xDot_t(4) = _vel.twist.angular.y - v_des.twist.angular.y;
	xDot_t(5) = _vel.twist.angular.z - v_des.twist.angular.z;
/*
	xDot_t(0) += v_des.twist.linear.x;
	xDot_t(1) +=v_des.twist.linear.y;
	xDot_t(2) += v_des.twist.linear.z;
	xDot_t(3) += v_des.twist.angular.x;
	xDot_t(4) +=  v_des.twist.angular.y;
	xDot_t(5) +=v_des.twist.angular.z;
*/
	xDot_t = -1*xDot_t; //inverti segno

	xDotDot(0) = a_des.accel.linear.x;
	xDotDot(1) = a_des.accel.linear.y;
	xDotDot(2) = a_des.accel.linear.z;
	xDotDot(3) = a_des.accel.angular.x;
	xDotDot(4) = a_des.accel.angular.y;
	xDotDot(5) = a_des.accel.angular.z;

}

void KUKA_INVDYN::compute_compliantFrame(const geometry_msgs::PoseStamped& p_des, const geometry_msgs::TwistStamped& v_des, const geometry_msgs::AccelStamped& a_des, const std::vector<double> alpha) {
	geometry_msgs::TwistStamped vmod_des;
	geometry_msgs::AccelStamped amod_des;

	if (alpha.size()<2) {
		vmod_des.twist.linear.x = alpha[0]*v_des.twist.linear.x;
		vmod_des.twist.linear.y = alpha[0]*v_des.twist.linear.y;
		vmod_des.twist.linear.z = alpha[0]*v_des.twist.linear.z;
		vmod_des.twist.angular.x = alpha[0]*v_des.twist.angular.x;
		vmod_des.twist.angular.y = alpha[0]*v_des.twist.angular.y;
		vmod_des.twist.angular.z = alpha[0]*v_des.twist.angular.z;
	}
	else {
		double alphaMin = alpha[0]*alpha[1];//min(alpha[0],alpha[1]);
		vmod_des.twist.linear.x = alphaMin*v_des.twist.linear.x;
		vmod_des.twist.linear.y = alphaMin*v_des.twist.linear.y;
		vmod_des.twist.linear.z = alphaMin*v_des.twist.linear.z;
		vmod_des.twist.angular.x = alphaMin*v_des.twist.angular.x;
		vmod_des.twist.angular.y = alphaMin*v_des.twist.angular.y;
		vmod_des.twist.angular.z = alphaMin*v_des.twist.angular.z;

		amod_des.accel.linear.x = alpha[1]*a_des.accel.linear.x;
		amod_des.accel.linear.y = alpha[1]*a_des.accel.linear.y;
		amod_des.accel.linear.z = alpha[1]*a_des.accel.linear.z;
		amod_des.accel.angular.x = alpha[1]*a_des.accel.angular.x;
		amod_des.accel.angular.y = alpha[1]*a_des.accel.angular.y;
		amod_des.accel.angular.z = alpha[1]*a_des.accel.angular.z;
		cout<<"alpha2: "<<alpha[1]<<endl;
	}

	compute_compliantFrame(p_des,vmod_des,amod_des);
}

void KUKA_INVDYN::compute_compliantFrame(const geometry_msgs::PoseStamped& p_des, const geometry_msgs::TwistStamped& v_des, const geometry_msgs::AccelStamped& a_des) {

	//cout<<_extWrench<<endl;
	//cout<<_Kdt<<endl;
	//cout<<_Kpt<<endl;
	if(!(_extWrench.norm()<1000000))
		_extWrench = Eigen::VectorXd::Zero(6);
	zDotDot_t = _Mt.inverse() * ( _extWrench - _Kdt*zDot_t - _Kpt*z_t);
	//cout<<_Mt(1,1)<<endl;
	zDotDot_t.tail(3) = Eigen::VectorXd::Zero(3);
	//cout<<zDotDot_t.transpose()<<endl;
	//zDotDot_t = Eigen::VectorXd::Zero(6);
	if(!_first_wrench) {
		zDotDot_t = Eigen::VectorXd::Zero(6);
	}
	zDot_t += zDotDot_t*_sTime;
	z_t += zDot_t*_sTime;
	//cout<<z_t.transpose()<<endl;

	geometry_msgs::PointStamped linDiff, linVelDiff;
	linDiff.header.stamp = ros::Time::now();
	linVelDiff.header.stamp = linDiff.header.stamp;
	linDiff.point.x = z_t(0);
	linDiff.point.y = z_t(1);
	linDiff.point.z = z_t(2);
	linVelDiff.point.x = zDot_t(0);
	linVelDiff.point.y = zDot_t(1);
	linVelDiff.point.z = zDot_t(2);
	_linearDifference_pub.publish(linDiff);
	_linearVelDifference_pub.publish(linVelDiff);

	_complAcc.accel.linear.x = a_des.accel.linear.x + zDotDot_t(0);
	_complAcc.accel.linear.y = a_des.accel.linear.y + zDotDot_t(1);
	_complAcc.accel.linear.z = a_des.accel.linear.z + zDotDot_t(2);
	_complAcc.accel.angular.x = a_des.accel.angular.x + zDotDot_t(3);
	_complAcc.accel.angular.y = a_des.accel.angular.y + zDotDot_t(4);
	_complAcc.accel.angular.z = a_des.accel.angular.z + zDotDot_t(5);

	_complVel.twist.linear.x = v_des.twist.linear.x + zDot_t(0);
	_complVel.twist.linear.y = v_des.twist.linear.y + zDot_t(1);
	_complVel.twist.linear.z = v_des.twist.linear.z + zDot_t(2);
	_complVel.twist.angular.x = v_des.twist.angular.x + zDot_t(3);
	_complVel.twist.angular.y = v_des.twist.angular.y + zDot_t(4);
	_complVel.twist.angular.z = v_des.twist.angular.z + zDot_t(5);

	_complPose.pose.position.x = p_des.pose.position.x + z_t(0);
	_complPose.pose.position.y = p_des.pose.position.y + z_t(1);
	_complPose.pose.position.z = p_des.pose.position.z + z_t(2);

	tf::Quaternion qe(p_des.pose.orientation.x,p_des.pose.orientation.y,p_des.pose.orientation.z,p_des.pose.orientation.w);
	tf::Quaternion qd(p_des.pose.orientation.x,p_des.pose.orientation.y,p_des.pose.orientation.z,p_des.pose.orientation.w);
	tf::Matrix3x3 Re_tf, Rd_tf;
	Eigen::Matrix3d Re,Rd;
	Re_tf.setRotation(qe);
	//Rd_tf.setRotation(qd);
	tf::matrixTFToEigen(Re_tf,Re);
	//	tf::matrixTFToEigen(Rd_tf,Rd);
	Eigen::Vector3d eps;
	eps << z_t(3),z_t(4),z_t(5);
	eps = Re.transpose()*eps;
	double eta = sqrt(1-eps(0)*eps(0)-eps(1)*eps(1)-eps(2)*eps(2));
	if(eta>1) eta=1;
	else if (eta<-1) eta=-1;
	double theta = 2*acos(eta);
	if(theta!=0) { //qd actually different from qe
		Eigen::Vector3d axis = (1.0/sin(theta*0.5))*eps;
		tf::Vector3 axis_tf;
		tf::vectorEigenToTF(axis,axis_tf);
		tf::Quaternion qerr(axis_tf,theta);
		tf::Matrix3x3 Rerr_tf(qerr);
		Eigen::Matrix3d Rerr;
		tf::matrixTFToEigen(Rerr_tf,Rerr);
		Rd = Re*Rerr;
		tf::matrixEigenToTF(Rd,Rd_tf);
		Rd_tf.getRotation(qd);
	}

	_complPose.pose.orientation.x = qd.x();
	_complPose.pose.orientation.y = qd.y();
	_complPose.pose.orientation.z = qd.z();
	_complPose.pose.orientation.w = qd.w();

	VectorXd vel;
	vel.resize(6);
	vel << _complVel.twist.linear.x ,
		_complVel.twist.linear.y ,
		_complVel.twist.linear.z ,
		_complVel.twist.angular.x,
		_complVel.twist.angular.y,
		_complVel.twist.angular.z;
	//_admittanceEnergy = ((zDot_t.head(3).transpose() * _Mt.topLeftCorner(3,3) * zDot_t.head(3)) + (z_t.head(3).transpose() * _Kpt.topLeftCorner(3,3) * z_t.head(3))).value();
	_admittanceEnergy = 0.5*zDot_t.dot(_Mt * zDot_t) + 0.5*z_t.dot(_Kpt * z_t);
	std_msgs::Float64 msg;
	msg.data = _admittanceEnergy;
	_forcesEnergy += zDot_t.dot(_extWrench) * _sTime;
	_robotEnergy_pub.publish(msg);
	double totalPower = zDot_t.dot(_extWrench) - zDot_t.dot(_Kdt * zDot_t);
	msg.data = totalPower;
	_totalPower_pub.publish(msg);


	_firstCompliant = true;
}

void KUKA_INVDYN::updatePose() {

	if(!_newPosReady)
		return;

	_desPose=_nextdesPose;
	_desVel = _nextdesVel;
	_desAcc = _nextdesAcc;

	_newPosReady = false;
}

void KUKA_INVDYN::updateForce() {
	if(!_newPosReady) return;

	_h_des=_nexth_des;
	_hdot_des = _nexthdot_des;
	_newPosReady = false;
}

bool KUKA_INVDYN::newTrajectory(const std::vector<geometry_msgs::PoseStamped> waypoints, const std::vector<double> times, const Eigen::VectorXd xdi, const Eigen::VectorXd xdf, const Eigen::VectorXd xddi, const Eigen::VectorXd xddf) {
	if(!_trajEnd) return false;

	_trajEnd=false;
	CARTESIAN_PLANNER	cplanner(_freq);
	cplanner.set_waypoints(waypoints,times,xdi,xdf,xddi,xddf);
	cplanner.compute();

	int trajsize = cplanner._x.size();
	int trajpoint = 0;
	double status = 0;

	_fControl = false;

	while(cplanner.isReady() && ros::ok()) {
		while(_newPosReady && ros::ok()) usleep(1);
		cplanner.getNext(_nextdesPose,_nextdesVel,_nextdesAcc);
		_newPosReady=true;
		trajpoint++;
		status = 100.0*((double)(trajpoint))/trajsize;
		if(_kukaActionServer.isActive()) {
			_actionFeedback.completePerc=status;
			_kukaActionServer.publishFeedback(_actionFeedback);
			if (_kukaActionServer.isPreemptRequested())
      			{
      			  ROS_INFO("ACTION Preempted");
      			  // set the action state to preempted
      			  _kukaActionServer.setPreempted();
							_trajEnd=true;
      			  return false;
      			}
		}
	}

	_trajEnd=true;
	return true;
}

bool KUKA_INVDYN::newTrajectory(const std::vector<geometry_msgs::PoseStamped> waypoints, const std::vector<double> times) {
	Eigen::VectorXd dummy(6);
	dummy = Eigen::VectorXd::Zero(6);
	newTrajectory(waypoints,times,dummy,dummy,dummy,dummy);
}

bool KUKA_INVDYN::newForceTrajectory(const std::vector<Eigen::VectorXd> waypoints, const std::vector<double> times, const Eigen::VectorXd mask) {

	if(!_trajEnd) return false;
	_trajEnd=false;

	_forceMask = mask;
	SPLINE_PLANNER* w[6];

	for(int i=0; i<6; i++) {
		w[i] = new SPLINE_PLANNER(_freq);
		std::vector<double> componentPoints;
		for(int j=0; j<waypoints.size(); j++) {
			componentPoints.push_back(waypoints[j](i));
		}
		w[i]->set_waypoints(componentPoints,times);
		w[i]->compute_traj();
	}

	xf(0) = _desPose.pose.position.x;
	xf(1) = _desPose.pose.position.y;
	xf(2) = _desPose.pose.position.z;
	xf(3) = _desPose.pose.orientation.x;
	xf(4) = _desPose.pose.orientation.y;
	xf(5) = _desPose.pose.orientation.z;
	xf(6) = _desPose.pose.orientation.w;
	xf_dot(0) = _desVel.twist.linear.x;
	xf_dot(1) = _desVel.twist.linear.y;
	xf_dot(2) = _desVel.twist.linear.z;
	xf_dot(3) = _desVel.twist.angular.x;
	xf_dot(4) = _desVel.twist.angular.y;
	xf_dot(5) = _desVel.twist.angular.z;

	_fControl=true;

	int trajsize = w[0]->_x.size();
	int trajpoint = 0;
	double status = 0;

	while(w[0]->isReady() && ros::ok()) {
		Eigen::VectorXd h(6), hdot(6);
		for(int i=0; i<6; i++) {
			double f, fdot, fdotdot;
			w[i]->getNext(f, fdot, fdotdot);
			h(i) = f;
			hdot(i) = fdot;
		}
		while(_newPosReady && ros::ok()) usleep(1);
		_nexth_des=h;
		_nexthdot_des=hdot;
		_newPosReady=true;
		trajpoint++;
		status = 100.0*((double)(trajpoint))/trajsize;
		if(_kukaActionServer.isActive()) {
			_actionFeedback.completePerc=status;
			_kukaActionServer.publishFeedback(_actionFeedback);
			if (_kukaActionServer.isPreemptRequested())
      {
        ROS_INFO("ACTION Preempted");
        // set the action state to preempted
        _kukaActionServer.setPreempted();
				_trajEnd=true;
        return false;
      }
		}
	}

	for(int i=0; i<6; i++)
		delete w[i];

	_trajEnd=true;
	return true;
}

void KUKA_INVDYN::compute_force_errors(const Eigen::VectorXd h, const Eigen::VectorXd hdot, const Eigen::VectorXd mask) {

	Eigen::VectorXd vel(6),acc(6),ht(6);
	double Kh = 5.0;

	ht = _extWrench-h;
	std_msgs::Float64 data;
	data.data=ht(1);
	_plannedwrench_pub.publish(data);
	cout<<"Error: "<<ht(1)<<" / "<<_extWrench(1)<<endl<<endl;
	//cout<<ht(1)<<endl<<endl;
	vel(0) = _complVel.twist.linear.x;
	vel(1) = _complVel.twist.linear.y;
	vel(2) = _complVel.twist.linear.z;
	vel(3) = _complVel.twist.angular.x;
	vel(4) = _complVel.twist.angular.y;
	vel(5) = _complVel.twist.angular.z;

	acc(0) = _complAcc.accel.linear.x;
	acc(1) = _complAcc.accel.linear.y;
	acc(2) = _complAcc.accel.linear.z;
	acc(3) = _complAcc.accel.angular.x;
	acc(4) = _complAcc.accel.angular.y;
	acc(5) = _complAcc.accel.angular.z;

	xf_dotdot = -_Kdt.inverse()*( -_Kpt*(vel-xf_dot) - Kh*ht + hdot);
	//xf_dotdot <<0,xf_dotdot(1),0,0,0,0;
	for (int i=0; i<6;i++) {
		if(mask(i)!=0)
			xf_dot(i) += xf_dotdot(i)*_sTime;
		else
			xf_dotdot(i) = 0.0;
	}
	for (int i=0; i<3;i++) {
		if(mask(i)!=0)
			xf(i) += xf_dot(i)*_sTime;
	}

	if((mask(3)!=0)||(mask(4)!=0)||(mask(5)!=0)) {
		Eigen::VectorXd wdes(3);
		wdes = xf_dot.tail(3);
		Eigen::Quaterniond quat(xf(6),xf(3),xf(4),xf(5));
		Eigen::AngleAxisd qax(quat);
		double theta = qax.angle();
		Eigen::Vector3d r = qax.axis();
		Eigen::Vector3d eps = sin(theta/2.0)*r;
		double eta = cos(theta/2.0);

		double eta_dot = -0.5*eps.transpose()*wdes;
		Eigen::Vector3d eps_dot = 0.5*(eta*Eigen::Matrix3d::Identity() - Skew(eps))*wdes;

		eta += eta_dot*_sTime;
		eps += eps_dot*_sTime;

		if(eta>1) eta=1;
		else if (eta<-1) eta=-1;
		theta = 2*acos(eta);
		if(theta!=0) { //qd actually different from qe
			r = (1.0/sin(theta*0.5))*eps;
			Eigen::AngleAxisd qax_new(theta,r);
			Eigen::Quaterniond q_new(qax_new);
			xf(3)=q_new.x();
			xf(4)=q_new.y();
			xf(5)=q_new.z();
			xf(6)=q_new.w();
		}
	}


	_nextdesPose.pose.position.x = xf(0);
	_nextdesPose.pose.position.y = xf(1);
	_nextdesPose.pose.position.z = xf(2);
	_nextdesPose.pose.orientation.x = xf(3);
	_nextdesPose.pose.orientation.y = xf(4);
	_nextdesPose.pose.orientation.z = xf(5);
	_nextdesPose.pose.orientation.w = xf(6);

	//xf_dot == Eigen::VectorXd::Zero(6);
	_nextdesVel.twist.linear.x = xf_dot(0);
	_nextdesVel.twist.linear.y = xf_dot(1);
	_nextdesVel.twist.linear.z = xf_dot(2);
	_nextdesVel.twist.angular.x = xf_dot(3);
	_nextdesVel.twist.angular.y = xf_dot(4);
	_nextdesVel.twist.angular.z = xf_dot(5);

	_nextdesAcc.accel.linear.x = xf_dotdot(0);
	_nextdesAcc.accel.linear.y = xf_dotdot(1);
	_nextdesAcc.accel.linear.z = xf_dotdot(2);
	_nextdesAcc.accel.angular.x = xf_dotdot(3);
	_nextdesAcc.accel.angular.y = xf_dotdot(4);
	_nextdesAcc.accel.angular.z = xf_dotdot(5);

}

void KUKA_INVDYN::run() {
	boost::thread ctrl_loop_t( &KUKA_INVDYN::ctrl_loop, this);
	//ros::spin();
}

void KUKA_INVDYN::actionCB(const kuka_control::waypointsGoalConstPtr &goal) {
	bool result;
	if(goal->poseOrForce) {
		std::vector<geometry_msgs::PoseStamped> waypoints = goal->waypoints.poses;
		std::vector<double> times = goal->times;
		Eigen::VectorXd xdi,xdf,xddi,xddf;

		twist2Vector(goal->initVel,xdi);
		twist2Vector(goal->finalVel,xdf);
		accel2Vector(goal->initAcc,xddi);
		accel2Vector(goal->finalAcc,xddf);

		result = newTrajectory(waypoints,times,xdi,xdf,xddi,xddf);
	} else {
		std::vector<Eigen::VectorXd> waypoints;
		std::vector<double> times = goal->times;
		Eigen::VectorXd initWrench(6),finalWrench(6), mask(6);

		wrench2Vector(goal->initWrench,initWrench);
		wrench2Vector(goal->finalWrench,finalWrench);
		wrench2Vector(goal->mask,mask);

		waypoints.push_back(initWrench);
		waypoints.push_back(finalWrench);

		result = newForceTrajectory(waypoints,times,mask);
	}

	if(result) {
      _actionResult.ok = true;
      ROS_INFO("ACTION: Succeeded");
      // set the action state to succeeded
      _kukaActionServer.setSucceeded(_actionResult);
  } else {
		_actionResult.ok = false;
		ROS_INFO("ACTION: Aborted. Probably already following another trajectory.");
		// set the action state to succeeded
		_kukaActionServer.setAborted(_actionResult);
	}
}


int main(int argc, char** argv) {
	ros::init(argc, argv, "iiwa_kdl");

	ros::AsyncSpinner spinner(1); // Use 1 thread
	spinner.start();

	KUKA_INVDYN iiwa(0.01);
	iiwa.run();
	ros::Rate r(50);
	diverterState state,oldState;
	state = iiwa.getState();
	oldState = state;

	while(ros::ok()) {
		state = iiwa.getState();
		if((state == DETACHED) && (oldState==HOOKED)) {
			ROS_WARN("Transition from HOOKED to DETACHED.");
			std::vector<geometry_msgs::PoseStamped> waypoints;
			geometry_msgs::PoseStamped p;
			iiwa.getDesPose(p);
			waypoints.push_back(p); //Initial
			p.pose.position.x = 0.5;
			p.pose.position.y = 0.0;
			p.pose.position.z = 0.4;
  			rotateYaw(p,p,M_PI/2);
  			waypoints.push_back(p); //medio
			p.pose.position.x = -0.041;
			p.pose.position.y = 0.65;//0.65
			p.pose.position.z = 0.50-0.30;//0.50-0.30
  			tf::Quaternion qinit(0.617,0.784,0.041,-0.038);
  			qinit.normalize();
			p.pose.orientation.z = qinit.z();
			p.pose.orientation.w = qinit.w();
			p.pose.orientation.x = qinit.x();
			p.pose.orientation.y = qinit.y();
  			waypoints.push_back(p); //finale
			std::vector<double> times;
  			times.push_back(0);
  			times.push_back(12);//15
			times.push_back(22);//30
			iiwa.newTrajectory(waypoints,times);
		}
		else if((state == IMPACT) && (oldState==DETACHED)) {
			ROS_WARN("Transition from DETACHED to IMPACT.");
			iiwa.setDone(false);
			//char c;
			//cin>>c;
			sleep(4);
			if(!ros::ok()) exit(0);
			std::vector<geometry_msgs::PoseStamped> waypoints;
			geometry_msgs::PoseStamped p;
			iiwa.getDesPose(p);
			waypoints.push_back(p); //Initial
			p.pose.position.z += 0.10;//0.30
			waypoints.push_back(p); //Initial
			std::vector<double> times;
			times.push_back(0);
  			times.push_back(1);
			
			iiwa.newTrajectory(waypoints,times);
			sleep(1);
			exit(0);
			//iiwa.setDone(true);
		}

		oldState=state;
		r.sleep();
	}

	ros::waitForShutdown();

	return 0;
}
