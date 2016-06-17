#include <gazebo/gazebo.hh>
#include <gazebo/physics/physics.hh>
#include <gazebo/common/common.hh>

#include <rtt/Component.hpp>
#include <rtt/Port.hpp>
#include <rtt/TaskContext.hpp>
#include <rtt/Logger.hpp>
#include <rtt/Property.hpp>
#include <rtt/Attribute.hpp>

//#include <Eigen/Dense>
#include <boost/graph/graph_concepts.hpp>
#include <rtt/os/Timer.hpp>
#include <rtt/os/TimeService.hpp>
#include <boost/thread/mutex.hpp>

#include <rci/dto/JointAngles.h>
#include <rci/dto/JointTorques.h>
#include <rci/dto/JointVelocities.h>

#include <nemo/Vector.h>
#include <nemo/Mapping.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
#include <algorithm>
#include <math.h>
#include <numeric>

#include "RealVector.h"
#include "ExtremeLearningMachine.h"

using namespace std;

#define l(lvl) RTT::log(lvl) << "[" << this->getName() << "] "



class UR5RttGazeboComponent: public RTT::TaskContext {
public:



	// BEST  Kp({2700 , 2700  , 2700 , 2700 , 2700 , 2700 }) , Ki({8.7 , 8.7  , 8.7  , 8.7  , 8.7  , 8.7 }) , Kd({209250 ,209250 , 209250 , 209250 , 209250 , 209250})

	UR5RttGazeboComponent(std::string const& name) :
	/*	RTT::TaskContext(name), nb_static_joints(0) , jnt_it(0) , jnt_width(0) , nb_links(0), inter_torque({{0} , {0} , {0} , {0} , {0} , {0}}) , jnt_effort(0),  torque_difference(0), control_value(0) , target_value(0), error_value(0),errorI(0), cumulative_error(0), error_derivative(0), last_error(0), dynStepSize(5) , pid_it(5) ,Kp({2000 , 3000  , 3000 , 540 , 540 , 1000 }) , Ki({0.4 , 0.4 , 0.4 , 0.2 , 0.2 , 0.4 }) , Kd({41850 ,41850 , 41850 , 41850 , 41850 , 37850}) , Ks({0,0,0,0,0,0}) */// HACK: The urdf has static tag for base_link, which makes it appear in gazebo as a joint.
			RTT::TaskContext(name), nb_static_joints(0) , ee_mass({0.00001 , 5 , 1 , 3}) , mass_id(0) , elm_id(0) , random_pos(false) , jnt_it(0) , jnt_width(0) , nb_links(0), inter_torque({{0} , {0} , {0} , {0} , {0} , {0}}) , inter_torque_elm({{0} , {0} , {0} , {0} , {0} , {0}}) ,nb_recording(0), jnt_effort(0),  torque_difference(0), control_value(0) , target_value(0), error_value(0),errorI(0), cumulative_error(0), error_derivative(0), last_error(0), dynStepSize(5) , pid_it(5) ,Kp({2000 , 3000  , 3000 , 540 , 540 , 70 }) , Ki({0.4 , 0.4 , 0.4 , 0.0 , 0.0 , 0.4 }) , Kd({41850 ,41850 , 11850 , 41850 , 51850 , 2850}) , Ks({0,0,0,0,0,0}) // HACK: The urdf has static tag for base_link, which makes it appear in gazebo as a joint.
			 {
		// Add required gazebo interfaces.
		this->provides("gazebo")->addOperation("configure",
				&UR5RttGazeboComponent::gazeboConfigureHook, this,
				RTT::ClientThread);
		this->provides("gazebo")->addOperation("update",
				&UR5RttGazeboComponent::gazeboUpdateHook, this, RTT::ClientThread);

		nb_iteration = 0;
		sim_id = 1;

		l1 = 0.7; // find real values later !!
		l2 = 0.9;// find real values later !!

		}


	double constrainCommand(double eff_max , double eff_min, double command)
		{
			if (command >= eff_max)
				return eff_max;
			else if (command <= eff_min)
				return eff_min;
			else
				return command;
		}

	void eeMass(double mass , gazebo::physics::ModelPtr model)
	{
		// Code to change mass and inertia tensor at the end effector during the simulation. // Here mass set to 1 for data recording.
		RTT::log(RTT::Error) << "Model modification." << RTT::endlog();
		auto inertial = model->GetLinks()[links_idx[nb_links-1]]->GetInertial();
		RTT::log(RTT::Error) << "Inertia pointer prepared." << RTT::endlog();
		inertial->SetMass(mass);
		double inertia_value;
		inertia_value = (mass*0.05*0.05)/6;
		RTT::log(RTT::Error) << "Mass prepared." << RTT::endlog();
		inertial->SetInertiaMatrix(inertia_value, inertia_value, inertia_value, 0, 0, 0);
		RTT::log(RTT::Error) << "Inertia matrix prepared." << RTT::endlog();
		model_links_[links_idx[nb_links-1]]->SetInertial(inertial);
		RTT::log(RTT::Error) << "Inertia matrix set." << RTT::endlog();
		model_links_[links_idx[nb_links-1]]->UpdateMass();
		RTT::log(RTT::Error) << "Inertia set to model. " << RTT::endlog();
	}


	//! Called from gazebo
	virtual bool gazeboConfigureHook(gazebo::physics::ModelPtr model) {
		if (model.get() == NULL) {
			RTT::log(RTT::Error) << "No model could be loaded" << RTT::endlog();
			std::cout << "No model could be loaded" << RTT::endlog();
			return false;
		}


		// ELM model creation.
		std::string infile("/homes/abalayn/workspace/rtt-gazebo-ur5-integration/elmmodel/data");
		elm=ExtremeLearningMachine::create(infile);
		RTT::log(RTT::Warning)  << "Generating testdata with dimensionality: " << elm->getInputDimension() << RTT::endlog();
		RTT::log(RTT::Warning) << "Expecting results with dimensionality: " << elm->getOutputDimension() << RTT::endlog();
		RTT::log(RTT::Error) << "ELM loaded" << RTT::endlog();




		// Get the joints
		gazebo_joints_ = model->GetJoints();
		model_links_ = model->GetLinks(); // Only working when starting gzserver and gzclient separately!

		RTT::log(RTT::Warning) << "Model has " << gazebo_joints_.size()
				<< " joints and " << model_links_.size()<< " links"<< RTT::endlog();

		//NOTE: Get the joint names and store their indices
		// Because we have base_joint (fixed), j0...j6, ati_joint (fixed)
		int idx = 0;
		for (gazebo::physics::Joint_V::iterator jit = gazebo_joints_.begin();
				jit != gazebo_joints_.end(); ++jit, ++idx) {

			const std::string name = (*jit)->GetName();
			// NOTE: Remove fake fixed joints (revolute with upper==lower==0
			// NOTE: This is not used anymore thanks to <disableFixedJointLumping>
			// Gazebo option (ati_joint is fixed but gazebo can use it )

			if ((*jit)->GetLowerLimit(0u) == (*jit)->GetUpperLimit(0u)) {
				RTT::log(RTT::Warning) << "Not adding (fake) fixed joint ["
						<< name << "] idx:" << idx << RTT::endlog();
				continue;
			}
			joints_idx.push_back(idx);
			joint_names_.push_back(name);
			RTT::log(RTT::Warning) << "Adding joint [" << name << "] idx:"
					<< idx << RTT::endlog();
			std::cout << "Adding joint [" << name << "] idx:" << idx
					<< RTT::endlog();
		}

		if (joints_idx.size() == 0) {
			RTT::log(RTT::Error) << "No Joints could be added, exiting"
					<< RTT::endlog();
			return false;
		}



		idx = 0;
		for (gazebo::physics::Link_V::iterator lit = model_links_.begin();
				lit != model_links_.end(); ++lit, ++idx) {

			const std::string name = (*lit)->GetName();
			links_idx.push_back(idx);
			link_names_.push_back(name);
			RTT::log(RTT::Warning) << "Adding link [" << name << "] idx:"
					<< idx << RTT::endlog();
			nb_links++;
		}



		if (links_idx.size() == 0) {
			RTT::log(RTT::Error) << "No links could be added, exiting"
					<< RTT::endlog();
			return false;
		}




		RTT::log(RTT::Warning) << "Done configuring gazebo" << RTT::endlog();

		for (unsigned j = 0; j < joints_idx.size(); j++)
		{
			gazebo_joints_[joints_idx[j]]->SetProvideFeedback(true);

			error_value.push_back(0);
			cumulative_error.push_back(0);
			last_error.push_back(0);
			errorI.push_back(0);
			error_derivative.push_back(0);
			control_value.push_back(0);
			target_value.push_back(0);
			torque_difference.push_back(0);
			jnt_it.push_back(1);
			jnt_width.push_back(0);
			thresholds.push_back(0);
			jnt_effort.push_back(0);
			nb_recording.push_back(0);


		}
		RTT::log(RTT::Warning) << "Done configuring PIDs" << RTT::endlog();

		data_file.open("/homes/abalayn/workspace/rtt-gazebo-ur5-integration/test_data.txt");
		if (!data_file)
			RTT::log(RTT::Error) << "The file could not be open." << RTT::endlog();
		error_file.open("/homes/abalayn/workspace/rtt-gazebo-ur5-integration/error_data.txt");
				if (!error_file)
					RTT::log(RTT::Error) << "The file could not be open." << RTT::endlog();



		target_value[0] = 0;
		target_value[1] = -0.1;
		target_value[2] =  3.14 - (+ target_value[1] + acos(sin(-target_value[1])*l1/l2) + 1.57) - 0.3 -0.4;
		target_value[3] = -3.14;
		target_value[4] = -1.4;
		target_value[5] = -1.57;

		jnt_width[0] = 6.28;
		jnt_width[1] = abs(-2.3 - -0.1);
		jnt_width[2] = abs(3.14 - (+ target_value[1] + acos(sin(-target_value[1])*l1/l2) + 1.57) - 0.3 -0.4 -  (3.14 - (+target_value[1] + acos(sin(-target_value[1])*l1/l2) + 1.57) - 3.14 + 0.8));
		jnt_width[3] = 0.7 - -3.14;
		jnt_width[4] = 1.57 - -1.4;
		jnt_width[5] = 3.14 - -1.57;
		RTT::log(RTT::Warning) << "Done configuring robot position" << RTT::endlog();


		/*// To launch data recording from a different start point.
		jnt_it[0] = 2;

			target_value[0] =  jnt_it[0]*jnt_width[0]/5 + (jnt_width[0]/5) * ((((float) rand()) / (float) RAND_MAX)); //0 ;
			RTT::log(RTT::Warning) << "Test trgt value 0: " <<  target_value[0] << RTT::endlog();
		 */

		thresholds[0] = 2;//31;
		thresholds[1] = 8;//12; //12;
		thresholds[2] = 2;//159; //30; // see if smaller could be ok. (if the position difference is not high).
		thresholds[3] = 2;//30;//29;
		thresholds[4] = 2;//29;//29;
		thresholds[5] = 10;//40;// 40;

		jnt_effort[0] = 150;
		jnt_effort[1] = 150;
		jnt_effort[2] = 150;
		jnt_effort[3] = 28;
		jnt_effort[4] = 28;
		jnt_effort[5] = 28;

		nb_recording[0] = 5.0;
		nb_recording[1] = 5.0;
		nb_recording[2] = 5.0;
		nb_recording[3] = 5.0;
		nb_recording[4] = 5.0;
		nb_recording[5] = 5.0;

		jnt_it[1] = -1;
		jnt_it[2] = -1;


		eeMass(5 , model);


		return true;
	}

	//! Called from Gazebo
	virtual void gazeboUpdateHook(gazebo::physics::ModelPtr model) {
		if (model.get() == NULL) {
			return;
		}
		nb_iteration++;


		/***************************************************************************************************************************************************/

		/*
		 * Data recording part
		*/

		if ((nb_iteration == 3410) || (nb_iteration == 3420) || (nb_iteration == 3430) || (nb_iteration == 3440) || (nb_iteration == 3450) || (nb_iteration == 3460) || (nb_iteration == 3470) || (nb_iteration == 3480) || (nb_iteration == 3490) || (nb_iteration == 3500) ) // To check if position is stable.
		{
			for (unsigned j = 0; j < joints_idx.size(); j++)
			{
				gazebo::physics::JointWrench w1 = gazebo_joints_[joints_idx[j]]->GetForceTorque(0u);
				gazebo::math::Vector3 a1 = gazebo_joints_[joints_idx[j]]->GetLocalAxis(0u);
				inter_torque[j].push_back(a1.Dot(w1.body1Torque));
			}
		}



		if (nb_iteration >= 3500) // For stabilisation of the torque.
		{


				data_file << "{ sim_id = " << sim_id << " ; ";
				double mean_of_torques = 0;
				for (unsigned j = 0; j < joints_idx.size(); j++)
				{
					data_file << "jnt " << j << " ; ";

					// Computing the mean of the torques.

					mean_of_torques = (std::accumulate((inter_torque[j]).begin(),(inter_torque[j]).end(), 0.0))/10.0;

					data_file << "trq "<< mean_of_torques << " ; ";

					data_file << "agl "	<< model->GetJoints()[joints_idx[j]]->GetAngle(0).Radian() << " ; ";
					data_file << "trg_agl "	<<target_value[j] << " ; ";
					mean_of_torques = 0;
					inter_torque[j] = {0};
				}
				data_file << " }" << std::endl;

			nb_iteration = 0;






		// Changes desired position  of each joint.
				if ((jnt_it[5]) < nb_recording[5])
				{
					jnt_it[5] = jnt_it[5]+1;

					if (random_pos)
					{
						target_value[5] = jnt_it[5]*jnt_width[5]/nb_recording[5] + (jnt_width[5]/nb_recording[5]) * ((((float) rand()) / (float) RAND_MAX)) -1.57;
					}
					else
					{
						target_value[5] = jnt_it[5]*(jnt_width[5]/nb_recording[5]) -1.57;
					}
				}
				else
				{

					if ((jnt_it[4]) < nb_recording[4])
					{
						if (random_pos)
							target_value[4] = jnt_it[4]*jnt_width[4]/nb_recording[4] + (jnt_width[5]/nb_recording[4]) * ((((float) rand()) / (float) RAND_MAX)) -1.4;
						else
							target_value[4] = jnt_it[4]*jnt_width[4]/nb_recording[4] -1.4;

						jnt_it[4]++;

					}
					else
					{
						if ( jnt_it[3] <nb_recording[3])
						{
							if (random_pos)
								target_value[3] = jnt_it[3]*jnt_width[3]/nb_recording[3] + (jnt_width[3]/nb_recording[3]) * ((((float) rand()) / (float) RAND_MAX)) -3.14;
							else
								target_value[3] = jnt_it[3]*jnt_width[3]/nb_recording[3] -3.14;
							jnt_it[3]++;
						}
						else
						{
							if (( jnt_it[2] > -(nb_recording[2]))&&(target_value[1] < 1.57))
							{
								if (random_pos)
									target_value[2] = jnt_it[2]*jnt_width[2]/nb_recording[2] + (jnt_width[2]/nb_recording[2]) * ((((float) rand()) / (float) RAND_MAX)) + 3.14 - (+ target_value[1] + acos(sin(-target_value[1])*l1/l2) + 1.57) - 0.3 -0.4;
								else
									target_value[2] = jnt_it[2]*jnt_width[2]/nb_recording[2]  + 3.14 - (+ target_value[1] + acos(sin(-target_value[1])*l1/l2) + 1.57) - 0.3 -0.4;
								jnt_it[2]--;
							}
							else if (( jnt_it[2] > -(nb_recording[2]))&&(target_value[1] > 1.57))
							{
								if (random_pos)
									target_value[2] = -jnt_it[2]*jnt_width[2]/nb_recording[2] - (jnt_width[2]/nb_recording[2]) * ((((float) rand()) / (float) RAND_MAX)) -3.14 + (- target_value[1] + 1.7 - acos(cos(-target_value[1]+1.7)*l1/l2)) + 0.3 +0.4;
								else
									target_value[2] = -jnt_it[2]*jnt_width[2]/nb_recording[2]  -3.14 + (- target_value[1] + 1.7 - acos(cos(-target_value[1]+1.7)*l1/l2)) + 0.3 +0.4;
								jnt_it[2]--;
							}
							else
							{
								if ((jnt_it[1] ) > -(nb_recording[1]))
								{
									if (random_pos)
										target_value[1] = jnt_it[1]*jnt_width[1]/nb_recording[1] - (jnt_width[1]/nb_recording[1]) * ((((float) rand()) / (float) RAND_MAX)) -0.1;
									else
										target_value[1] = jnt_it[1]*jnt_width[1]/nb_recording[1] -0.1;

									jnt_width[2] = abs(3.14 - (+ target_value[1] + acos(sin(-target_value[1])*l1/l2) + 1.57) - 0.3 -0.4 -  (3.14 - (+target_value[1] + acos(sin(-target_value[1])*l1/l2) + 1.57) - 3.14 + 0.8));
									jnt_it[1]--;
								}
								else
								{
									target_value[1] = -0.1;
									jnt_it[1] = -1;
									jnt_width[2] = abs(3.14 - (+ target_value[1] + acos(sin(-target_value[1])*l1/l2) + 1.57) - 0.3 -0.4 -  (3.14 - (+target_value[1] + acos(sin(-target_value[1])*l1/l2) + 1.57) - 3.14 + 0.8));

									if (jnt_it[0] >= nb_recording[0])
									{
										target_value[0] = 0;
										jnt_it[0] = 1;

										mass_id++;
										if (mass_id >= ee_mass.size())
										{
											mass_id = 0;
										}
										eeMass(ee_mass[mass_id] , model);
										RTT::log(RTT::Error) << "Mass set to " << ee_mass[mass_id] << " kg. " << RTT::endlog();

									}
									else
									{
										if (random_pos)
											target_value[0] = jnt_it[0]*jnt_width[0]/nb_recording[0] + (jnt_width[0]/nb_recording[0]) * ((((float) rand()) / (float) RAND_MAX));
										else
											target_value[0] = jnt_it[0]*jnt_width[0]/nb_recording[0];
										jnt_it[0]++;
									}
								}
								if (target_value[1] < 1.57)
									target_value[2] = 3.14 - (+ target_value[1] + acos(sin(-target_value[1])*l1/l2) + 1.57) - 0.3 -0.4;
								else
									target_value[2] = -3.14 + (- target_value[1] + 1.7 - acos(cos(-target_value[1]+1.7)*l1/l2)) + 0.3 +0.4;
								jnt_it[2] = -1;
							}
							target_value[3] = -3.14;
							jnt_it[3] = 1;
						}
						target_value[4] = -1.4;
						jnt_it[4] = 1;
					}
					target_value[5] = -1.57;
					jnt_it[5] = 1;
				}



}




		/***************************************************************************************************************************************************/


		/***************************************************************************************************************************************************/


	//	/*
	//	 * Video recording part
	//	 */
	//	/*
	//	if (sim_id < 5000)
	//	{
	//		target_value[0] = -1;
	//		target_value[1] = -0.7;
	//		target_value[2] = 1.3;
	//		target_value[3] = -2;
	//		target_value[4] = -1.4;
	//		target_value[5] = -1.57;
	//	}
	//	else if ((sim_id  > 5000) && (sim_id < 6500))
	//	{
	//
	//		target_value[2] = target_value[2] - 0.0006;
	//
	//	}
	//	else if ((sim_id  > 6500) && (sim_id < 10200))
	//	{
	//		target_value[0] = target_value[0] - 0.0006;
	//
	//	}
	//	else if ((sim_id  > 10200) && (sim_id < 11700))
	//	{
	//		target_value[2] = target_value[2] + 0.0006;
	//	}
	//
	//
	//	target_value[0] = -1;
	//	target_value[1] = -1.57;
	//	target_value[2] = 1.57;
	//	target_value[3] = -1.57;
	//	target_value[4] = -1.57;
	//	target_value[5] = -1.57;
	//	 */

		/***************************************************************************************************************************************************/



		/***************************************************************************************************************************************************/

		/*
		 * ELM part:
		 * Computing the difference between current torque and awaited torque for each joint.
		 */



		elm_id++;

		if ((elm_id == 910) || (elm_id == 920) || (elm_id == 930) || (elm_id == 940) || (elm_id == 950) || (elm_id == 960) || (elm_id == 970) || (elm_id == 980) || (elm_id == 990) || (elm_id == 1000) ) // To check if position is stable.
		{
			for (unsigned j = 0; j < joints_idx.size(); j++)
			{
				gazebo::physics::JointWrench w1 = gazebo_joints_[joints_idx[j]]->GetForceTorque(0u);
				gazebo::math::Vector3 a1 = gazebo_joints_[joints_idx[j]]->GetLocalAxis(0u);
				inter_torque_elm[j].push_back(a1.Dot(w1.body1Torque));
			}
		}
		/*
		// Create vector containing awaited position.
		RealVectorPtr inputdata = RealVector::create(elm->getInputDimension(), 0.0);
		for (int j=0; j<inputdata->getDimension(); j++) inputdata->setValueEquals(j,model->GetJoints()[joints_idx[j]]->GetAngle(0).Radian());

		// Create vector containing the torque which should be applied.
		RealVectorPtr result = elm->evaluate(inputdata);
		//RTT::log(RTT::Warning) << "Evaluating [" << inputdata << "] -> [" <<  result << "]" << RTT::endlog();

		error_file << "{" ;

		for (unsigned j = 0; j < joints_idx.size(); j++)
		{
			// Compute current torque.
			gazebo::physics::JointWrench w1 = gazebo_joints_[joints_idx[j]]->GetForceTorque(0u);
			gazebo::math::Vector3 a1 = gazebo_joints_[joints_idx[j]]->GetLocalAxis(0u);

			torque_difference[j] = result->getValue(j) - (a1.Dot(w1.body1Torque));
			//RTT::log(RTT::Warning) << "Torque difference: " << torque_difference[j] << RTT::endlog();
			error_file << "joint " << j << ": " << torque_difference[j] << " dsrTrq: " << result->getValue(j) << " realTrq: " << (a1.Dot(w1.body1Torque)) << " ;";
		}
		error_file << "}" << std::endl;
		*/

		/***************************************************************************************************************************************************/



		/***************************************************************************************************************************************************/
		/*
		 * Compliancy of the robot.
		 */


//	if ((sim_id > 3500)&&(elm_id >= 1000))
//	{
//	elm_id = 0;
//
//
//
//
//					// ********************************************************************************************************************************
//				// * ELM part:
//				// * Computing the difference between current torque and awaited torque for each joint.
//				 // Create vector containing awaited position.
//
//				RealVectorPtr inputdata = RealVector::create(elm->getInputDimension(), 0.0);
//				for (int j=0; j<inputdata->getDimension(); j++) inputdata->setValueEquals(j,model->GetJoints()[joints_idx[j]]->GetAngle(0).Radian());
//
//				// Create vector containing the torque which should be applied.
//				RealVectorPtr result = elm->evaluate(inputdata);
//
//					error_file << "{" ;
//
//		for (unsigned j = 0; j < joints_idx.size(); j++)
//		{
//
//
//			double mean_of_torques_elm = 0;
//			mean_of_torques_elm = (std::accumulate((inter_torque_elm[j]).begin(),(inter_torque_elm[j]).end(), 0))/10.0;
//
//
//			torque_difference[j] = result->getValue(j) - mean_of_torques_elm;
//			//RTT::log(RTT::Warning) << "Torque difference: " << torque_difference[j] << RTT::endlog();
//			error_file << "joint " << j << ": " << torque_difference[j] << " dsrTrq: " << result->getValue(j) << " realTrq: " <<  mean_of_torques_elm << " ;";
//
//				// ********************************************************************************************************************************
//
//
//
//
//
//			// ********************************************************************************************************************************
//				// * Compliance part:
//
//
//			if (abs(torque_difference[j]) > thresholds[j])
//			{
//			//	gazebo::physics::JointWrench w1 = gazebo_joints_[joints_idx[j]]->GetForceTorque(0u);
//			//	gazebo::math::Vector3 a1 = gazebo_joints_[joints_idx[j]]->GetLocalAxis(0u);
//			//	error_file << "{" ;
//			//	error_file << "joint " << j << ": " << torque_difference[j] << " dsrTrq: " << result->getValue(j) << " realTrq: " << (a1.Dot(w1.body1Torque)) << " ;";
//			//	error_file << "tgtPos" << target_value[j];
//		// Test: the new target value is the current position. 	To be evaluated because there is no deformation compared to soft robots!
		//	target_value[j] = model->GetJoints()[joints_idx[j]]->GetAngle(0).Radian();
//			//	error_file << "newPos" << target_value[j];
//			//	error_file << "}" << std::endl;
//
//
		// Test: the new target value: we add an angle: has to be set for each joint>
//				if (torque_difference[j] > 0)
//					target_value[j] = target_value[j] + 0.2;
//				else
//					target_value[j] = target_value[j] - 0.2;
//
//				RTT::log(RTT::Warning) << "Joint " << j << "set to " << target_value[j] << RTT::endlog();
//			}
//			inter_torque_elm[j] = {0};
//			mean_of_torques_elm = 0;
//
//		}
//				error_file << "}" << std::endl;
//
//	}

		/***************************************************************************************************************************************************/



		/***************************************************************************************************************************************************/

		/*
		 *  Try to guess which payload is currently at the end-effector.
		 */



		if ((sim_id == 3410) || (sim_id == 3420) || (sim_id == 3430) || (sim_id == 3440) || (sim_id == 3450) || (sim_id == 3460) || (sim_id == 3470) || (sim_id == 3480) || (sim_id == 3490) || (sim_id == 3500) ) // To check if position is stable.
				{
					for (unsigned j = 0; j < joints_idx.size(); j++)
					{
						gazebo::physics::JointWrench w1 = gazebo_joints_[joints_idx[j]]->GetForceTorque(0u);
						gazebo::math::Vector3 a1 = gazebo_joints_[joints_idx[j]]->GetLocalAxis(0u);
						inter_torque[j].push_back(a1.Dot(w1.body1Torque));
					}
				}
		if (sim_id == 3500)
		{
			RealVectorPtr inputdata = RealVector::create(elm->getInputDimension(), 0.0);
			for (int j=0; j<inputdata->getDimension(); j++) inputdata->setValueEquals(j,model->GetJoints()[joints_idx[j]]->GetAngle(0).Radian());
			// Create vector containing the torque which should be applied.
			RealVectorPtr result = elm->evaluate(inputdata);
			for (unsigned j = 0; j < joints_idx.size(); j++)
			{
				double mean_of_torques = 0;
				mean_of_torques= (std::accumulate((inter_torque[j]).begin(),(inter_torque[j]).end(), 0))/10.0;
				torque_difference[j] = result->getValue(j) - mean_of_torques;

				//Here, we have to guess the mass. See data to observe the evolution of every mass for one position!

				mean_of_torques = 0;
				inter_torque[j] = {0};
			}

		}









		/***************************************************************************************************************************************************/





		/***************************************************************************************************************************************************/
		/*
		 * PID Component part
		 */

		pid_it++;

		// PID control of position with torque
		if (pid_it >= dynStepSize)
		{
			for (unsigned j = 0; j < joints_idx.size(); j++)
			{
				//Regular PID
			/*
				error_value[j] = target_value[j] -  model->GetJoints()[joints_idx[j]]->GetAngle(0).Radian();
				error_derivative[j] = error_value[j]-last_error[j];
				cumulative_error[j] = cumulative_error[j] + error_value[j];
				control_value[j] = constrainCommand(jnt_effort[j] , -jnt_effort[j] , error_value[j]*Kp[j] + cumulative_error[j]*Ki[j]*dynStepSize + error_derivative[j]*(Kd[j]/dynStepSize));
				last_error[j] = error_value[j];
				pid_it = 0;
			*/

				// PID anti wind-up

				error_value[j] = target_value[j] -  model->GetJoints()[joints_idx[j]]->GetAngle(0).Radian();
				error_derivative[j] = error_value[j]-last_error[j];
				control_value[j] = constrainCommand( jnt_effort[j], -jnt_effort[j] , Kp[j]*error_value[j] + (Kd[j]/dynStepSize)*error_derivative[j] + errorI[j]);
				errorI[j] = errorI[j] + dynStepSize*(Ki[j]*error_value[j] + Ks[j]*(control_value[j] - Kp[j]*error_value[j] - (Kd[j]/dynStepSize)*error_derivative[j] - errorI[j] ));
				last_error[j] = error_value[j];
				pid_it = 0;

			//	RTT::log(RTT::Error) << j << " " << error_value[j] << "  " << control_value[j] << "  " << errorI[j] << RTT::endlog() ;
			}



		// For tuning PID.
		//RTT::log(RTT::Error) << "Ki " << Kd[1]  << " agl0 "	<< model->GetJoints()[joints_idx[0]]->GetAngle(0).Radian() <<" trg_agl1 "	<<target_value[1] <<  " agl1 "	<< model->GetJoints()[joints_idx[1]]->GetAngle(0).Radian() <<  " trg_agl2 "	<<target_value[2] << " agl2 "	<< model->GetJoints()[joints_idx[2]]->GetAngle(0).Radian() << RTT::endlog();

		}

		for (unsigned j = 0; j < joints_idx.size(); j++)
		{
			gazebo_joints_[joints_idx[j]]->SetForce(0 , control_value[j]);
			//RTT::log(RTT::Error) << j << " " << target_value[j]  << RTT::endlog() ;

		}

		/***************************************************************************************************************************************************/


		/*
		 * Test for mass modification at the end-effector.
		 */

		/*
		if (sim_id == 8000)
		{
			RTT::log(RTT::Error) << "entering mass modification " << RTT::endlog() ;
			eeMass(5.0 , model);
		}
		*/

		sim_id ++;


	}



	virtual bool configureHook() {
		return true;
	}


	virtual void updateHook() {
		return;
	}

protected:

	//! Synchronization ??

	// File where data are written.
	std::ofstream data_file;
	std::ofstream error_file;

	bool random_pos;
	std::vector<double> nb_recording;
	std::vector<double> ee_mass;
	int mass_id;

	int nb_iteration; // number of hook iterations for one tested position.
	int sim_id; // number of angle positions tested.

	int elm_id;

	std::vector<int> joints_idx;
	std::vector<int> links_idx;


	std::vector<gazebo::physics::JointPtr> gazebo_joints_;
	gazebo::physics::Link_V model_links_;
	std::vector<std::string> joint_names_;
	std::vector<std::string> link_names_;
	int nb_links;

	int nb_static_joints;

	double l1; // length of link1
	double l2;  // length of link2


	// For recording data randomly.
	std::vector<double> jnt_it;
	std::vector<double> jnt_width; // Shouldn't forget to modify it for joint 2!!

	// Variable to save intermediate robot position - to decide if the data will be written in the file.
	std::vector< std::vector<double> > inter_torque;

	// Variables for PID controller : transform to vector for several joints.
	std::vector<double> error_value;
	std::vector<double> cumulative_error;
	std::vector<double> last_error;
	double dynStepSize;
	std::vector<double> Kp;
	std::vector<double> Kd;
	std::vector<double> Ki;
	std::vector<double> Ks;
	std::vector<double> control_value;
	std::vector<double> target_value;
	std::vector<double> errorI;
	std::vector<double> error_derivative;


	int pid_it;


	// ELM Learner
	ExtremeLearningMachinePtr elm;
	std::vector<double> torque_difference;
	// Variable to save intermediate robot position - to decide if the data will be written in the file.
	std::vector< std::vector<double> > inter_torque_elm;

	// Compliancy
	std::vector<double> thresholds;
	std::vector<double> jnt_effort;


};

ORO_LIST_COMPONENT_TYPE(UR5RttGazeboComponent)
ORO_CREATE_COMPONENT_LIBRARY();


