/**
 * Minimal program to open a D415/D435 sensor with specific settings, then toggle
 * its auto exposure and region of interest settings at runtime.
 *
 * To keep the code smaller and capable of running on minimal systems, no GUI
 * is used at all.
 */

#include <iostream>
#include <string.h>
#include <stdint.h>
#include <chrono>
#include <list>
#include <thread>

#ifdef WIN32
#include <Windows.h>
#else
#include <csignal>
#endif

#include <librealsense2/rs.hpp>
#include <librealsense2/rs_advanced_mode.hpp>

// Contains a long JSON string specifying camera settings
#include "realsensesettings.h"

bool got_sigint = false;
const int color_w = 960;
const int color_h = 540;
const int depth_w = 640;
const int depth_h = 480;

/**
 * Wrapper to call delete or delete[] on dtor
 */
template <class T>
class DeleteLater {
public:
	DeleteLater(T* p, bool is_arr, std::string n) {
		m_p = p;
		m_arr = is_arr;
		m_name = n;
	}

	virtual ~DeleteLater() {
		if (!m_p)
			return;

		std::cout << "freeing memory: " << m_name << std::endl;

		if (m_arr) {
			delete[] m_p;
		} else {
			delete m_p;
		}
	}

	T* m_p;
	bool m_arr;
	std::string m_name;
};

void stop(rs2::pipeline& p) {
	p.stop();
}

#ifdef WIN32
bool sigint_handler(DWORD fdwCtrlType) {
	if(fdwCtrlType == CTRL_C_EVENT) {
		std::cout << "signal caught: " << fdwCtrlType<< std::endl;
		got_sigint = true;
		return true;
	}

	return false;
}
#else
void sigint_handler(int sig)
{
	std::cout << "signal caught: " << sig << std::endl;
	got_sigint = true;
}
#endif

int main() try {

	// register signal handlers
#ifdef WIN32
	SetConsoleCtrlHandler( (PHANDLER_ROUTINE) sigint_handler, TRUE );
#else
	signal(SIGINT, sigint_handler);
#endif

	// allocate buffers for reading color and depth frames
	unsigned char* colorbuf = new unsigned char[color_w * color_h * 3];
	if (colorbuf == NULL) {
		std::cout << "failed allocating colorbuf" << std::endl;
		return 1;
	}

	DeleteLater<unsigned char> del1(colorbuf, true, "colorbuffer");

	uint16_t* depthbuf = new uint16_t[depth_w * depth_h];
	if (depthbuf == NULL) {
		std::cout << "failed allocating depthbuf" << std::endl;
		return 1;
	}

	DeleteLater<uint16_t> del2(depthbuf, true, "depthbuffer");

	memset(colorbuf, 0, color_w*color_h*3);
	memset(depthbuf, 0, depth_w*depth_h*sizeof(uint16_t));

	std::cout << "Allocated memory" << std::endl;

	rs2::context context;

	// Create a Pipeline - this serves as a top-level API for streaming and processing frames
	rs2::pipeline pipeline(context);
	std::shared_ptr<rs2_pipeline> p_pipeline = std::shared_ptr<rs2_pipeline>(pipeline);

	std::cout << "Created pipeline" << std::endl;

	rs2::device_list devs = context.query_devices();
	if (devs.size() != 1) {
		std::cout << "Expecting to find one device connected to the computer" << std::endl;
		return 1;
	}

	rs2::device dev = devs[0];
	const char* serial = dev.get_info(RS2_CAMERA_INFO_SERIAL_NUMBER);
	std::cout << "Using camera: " << serial << std::endl;

	std::vector<rs2::sensor> sensors = dev.query_sensors();
	std::cout << "Device has " << sensors.size() << " sensors" << std::endl;

	rs400::advanced_mode adv(dev);
	if (!adv.is_enabled()) {
		std::cout << "advanced mode is not enabled -> enabling it" << std::endl;

		adv.toggle_advanced_mode(true);
		std::cout << "Finished toggling advanced mode" << std::endl;
	} else {
		std::cout << "advanced mode is already enabled" << std::endl;
	}

	try {
		// This variable is in an external file due to it being a very long string
		adv.load_json(realsense_advanced_settings_json);
	} catch (const rs2::error& e) {
		std::cout << "RealSense error calling " << e.get_failed_function()
			<< "(" << e.get_failed_args() << "):\n " << e.what() <<
			" when loading settings JSON." << std::endl;
	}

	// Enable max resolution streams
	rs2::config conf;

	conf.enable_device(serial);
	conf.enable_stream(RS2_STREAM_DEPTH, -1, depth_w, depth_h, RS2_FORMAT_Z16, 30);
	conf.enable_stream(RS2_STREAM_COLOR, -1, color_w, color_h, RS2_FORMAT_RGB8, 30);

	std::cout << "streams enabled" << std::endl;

	uint64_t frames_got = 0;

	// Configure and start the pipeline
	rs2::pipeline_profile prof = pipeline.start(conf);
	std::cout << "pipeline started" << std::endl;

	std::cout << "entering main loop" << std::endl;

	std::list<int> ftimes;
	int dur_sum = 0;

	std::chrono::high_resolution_clock::time_point t_since_toggle = std::chrono::high_resolution_clock::now();
	int64_t next_toggle = 3000;

	while (true) {

		std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();

		if (got_sigint) {
			break;
		}

		// Block program until frames arrive
		rs2::frameset frames = pipeline.wait_for_frames(3000);

		bool got_depth = false;
		bool got_color = false;

		// get specific frame instances
		for (auto&& f : frames) {
			if (f.is<rs2::depth_frame>()) {
				rs2::depth_frame dframe = f.as<rs2::depth_frame>();
				int d_width = dframe.get_width();
				int d_height = dframe.get_height();

				if (d_width != depth_w || d_height != depth_h) {

					std::cout << "Invalid depth frame resolution: "
						<< d_width << ", " << d_height << std::endl;

					stop(pipeline);
					return 1;
				}

				uint16_t* depthdata = (uint16_t*)dframe.get_data();
				memcpy(depthbuf, depthdata, depth_w * depth_h * sizeof(uint16_t));
				got_depth = true;

			} else if (f.is<rs2::video_frame>()) {
				rs2::video_frame cframe = f.as<rs2::video_frame>();
				int c_width = cframe.get_width();
				int c_height = cframe.get_height();

				if (c_width != color_w || c_height != color_h) {
					std::cout << "Invalid color frame resolution: "
						<< c_width << ", " << c_height << std::endl;
					stop(pipeline);
					return 1;
				}

				unsigned char* colordata = (unsigned char*)cframe.get_data();
				memcpy(colorbuf, colordata, color_w * color_h * 3);
				got_color = true;
			}
		}

		if (!got_color || !got_depth) {
			std::cout << "Did not get all frame types" << std::endl;
			break;
		}

		frames_got++;

		std::chrono::high_resolution_clock::time_point t2 = std::chrono::high_resolution_clock::now();
		auto toggle = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t_since_toggle).count();

		if (toggle > next_toggle) {

			// Disable auto exposure, re-enable it, specify region of interest
			t_since_toggle = t2;
			next_toggle = next_toggle + 10000;

			for (auto& s : sensors) {

				try {
					if (!s.supports(RS2_OPTION_ENABLE_AUTO_EXPOSURE) ||
						!s.supports(RS2_OPTION_EMITTER_ENABLED) ||
						!s.is<rs2::roi_sensor>()) {
						continue;
					}
				} catch (const rs2::error& e) {
					std::cout << "RealSense error calling " << e.get_failed_function()
						<< "(" << e.get_failed_args() << "):\n " << e.what() <<
						" when testing if sensor is depth sensor." << std::endl;
					continue;
				}

				try {
					std::cout << "Getting auto exposure state (1)" << std::endl;
					float aexp = s.get_option(RS2_OPTION_ENABLE_AUTO_EXPOSURE);

					if (aexp != 0.f) {
						std::cout << "Setting auto exposure off" << std::endl;
						s.set_option(RS2_OPTION_ENABLE_AUTO_EXPOSURE, 0.0f);
						std::this_thread::sleep_for(std::chrono::seconds(3));
					} else {
						std::cout << "Auto exposure is already off, no need to disable: " << aexp << std::endl;
					}
				} catch (const rs2::error& e) {
					std::cout << "RealSense error calling " << e.get_failed_function()
						<< "(" << e.get_failed_args() << "):\n " << e.what() <<
						" when disabling auto exposure." << std::endl;
				}

				try {
					std::cout << "Setting auto exposure on" << std::endl;
					s.set_option(RS2_OPTION_ENABLE_AUTO_EXPOSURE, 1.0f);
					std::this_thread::sleep_for(std::chrono::seconds(3));
				} catch (const rs2::error& e) {
					std::cout << "RealSense error calling " << e.get_failed_function()
						<< "(" << e.get_failed_args() << "):\n " << e.what() <<
						" when toggling device auto exposure settings." << std::endl;
				}

				try {
					std::cout << "Getting auto exposure state (2)" << std::endl;
					float aexp = s.get_option(RS2_OPTION_ENABLE_AUTO_EXPOSURE);
					std::cout << "auto exposure value before ROI request: " << aexp << std::endl;
					if (aexp == 0.0f) {
						std::cout << "Cannot set ROI: auto exposure failed to re-enable" << std::endl;
						continue;
					}

					rs2::region_of_interest ri;
					ri.min_x = depth_w*0.4f;
					ri.max_x = depth_w*0.6f;
					ri.min_y = depth_h*0.4f;
					ri.max_y = depth_h*0.6f;

					std::cout << "Set region of interest" << std::endl;
					s.as<rs2::roi_sensor>().set_region_of_interest(ri);
					std::this_thread::sleep_for(std::chrono::seconds(1));
				} catch (const rs2::error& e) {
					std::cout << "RealSense error calling " << e.get_failed_function()
						<< "(" << e.get_failed_args() << "):\n " << e.what() <<
						" when setting auto exposure region of interest." << std::endl;
				}

				try {
					std::cout << "Enabling emitter" << std::endl;
					s.set_option(RS2_OPTION_EMITTER_ENABLED, 1.0f);
					std::this_thread::sleep_for(std::chrono::seconds(1));
				} catch (const rs2::error& e) {
					std::cout << "RealSense error calling " << e.get_failed_function()
						<< "(" << e.get_failed_args() << "):\n " << e.what() <<
						" when enabling emitter." << std::endl;
				}
			}
		}

		// calculate frame time
		auto dur_frame = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
		if (dur_frame == 0) {
			dur_frame = 1;
		}

		// calculate average fps
		ftimes.push_back(dur_frame);

		if (ftimes.size() > 100) {
			dur_sum -= ftimes.front();
			ftimes.pop_front();
		}

		dur_sum += dur_frame;
		int avg_dur = dur_sum / ftimes.size();

		if (dur_sum == 0) {
			dur_sum = 1;
		}

		int avg_fps = 1000 / avg_dur;

		std::cout << "Finished frame " << frames_got << " in " << dur_frame
			<< " milliseconds (" << avg_fps << " fps)" << std::endl;
	}

	std::cout << "exited main loop" << std::endl;

	stop(pipeline);
	std::cout << "pipeline stopped" << std::endl;

	return 0;
} catch (const rs2::error & e) {
	std::cerr << "main exception handler: RealSense error calling " << e.get_failed_function()
		<< "(" << e.get_failed_args() << "):\n	  " << e.what() << std::endl;
	return 1;
} catch (const std::exception& e) {
	std::cerr << "Unspecified exception: " << e.what() << std::endl;
	return 1;
}

