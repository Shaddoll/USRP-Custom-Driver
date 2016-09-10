#include <uhd/utils/thread_priority.hpp>
#include <uhd/utils/safe_main.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/exception.hpp>
#include <boost/program_options.hpp>
#include <boost/format.hpp>
#include <boost/thread.hpp>
#include <boost/chrono.hpp>
#include <iostream>
#include <fstream>
#include <complex>
#include <csignal>
#include <cstdlib>

namespace po = boost::program_options;

static bool stop_signal_called = false;
void sig_int_handler(int){
    stop_signal_called = true;
}

template<typename samp_type> void recv_to_file(
    uhd::usrp::multi_usrp::sptr usrp,
    const std::string &cpu_format,
    const std::string &file,
    size_t samps_per_buff,
    double seconds_in_future,
    int num_requested_samples
){
    int num_total_samps = 0;
    unsigned int num_channels = usrp->get_rx_num_channels();
    uhd::stream_args_t stream_args(cpu_format, "sc16");
    for (size_t chan = 0; chan < num_channels; ++chan) {
        stream_args.channels.push_back(chan);
    }
    uhd::rx_streamer::sptr rx_stream = usrp->get_rx_stream(stream_args);
    uhd::rx_metadata_t md;
    std::vector<samp_type *>buff(num_channels);
    std::ofstream outfiles[num_channels];

    for (unsigned int i = 0; i < num_channels; ++i) {
      buff[i] = new samp_type[samps_per_buff];
      std::string rx_file = file + "_" + boost::lexical_cast<std::string>(i) + ".dat";
      outfiles[i].open(rx_file.c_str(), std::ofstream::binary);
      std::cout << boost::format("Channel %u: Writing to file %s...\n") % i % rx_file;
    }

    bool overflow_message = true;

    uhd::stream_cmd_t stream_cmd((num_requested_samples == 0)?
        uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS:
        uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE
    );
    //uhd::stream_cmd_t stream_cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
    stream_cmd.num_samps = num_requested_samples;
    //stream_cmd.num_samps = 0;
    stream_cmd.stream_now = false;
    stream_cmd.time_spec = usrp->get_time_now() + uhd::time_spec_t(seconds_in_future);
    rx_stream->issue_stream_cmd(stream_cmd);

    while(not stop_signal_called and (num_requested_samples > num_total_samps or num_requested_samples == 0)){
            size_t num_rx_samps = rx_stream->recv(
            buff, samps_per_buff, md,
            seconds_in_future + 4.0
        );
        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) {
            std::cout << boost::format("Timeout while streaming") << std::endl;
            break;
        }
        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW){
            if (overflow_message){
                overflow_message = false;
                std::cerr << boost::format(
                    "Got an overflow indication. Please consider the following:\n"
                    "  Your write medium must sustain a rate of %fMB/s.\n"
                    "  Dropped samples will not be written to the file.\n"
                    "  Please modify this example for your purposes.\n"
                    "  This message will not appear again.\n"
                ) % (usrp->get_rx_rate()*sizeof(samp_type)/1e6);
            }
            continue;
        }
        if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE){
            throw std::runtime_error(str(boost::format(
                "Unexpected error code 0x%x"
            ) % md.error_code));
        }
        num_total_samps += num_rx_samps;
	    for (unsigned int i = 0; i < num_channels; ++i) {
	        outfiles[i].write((const char*)(buff[i]), num_rx_samps*sizeof(samp_type));
	    }
    }
    stream_cmd.stream_mode = uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;
    rx_stream->issue_stream_cmd(stream_cmd);
    for (unsigned int i = 0; i < num_channels; ++i) {
      outfiles[i].close();
    }
}

int UHD_SAFE_MAIN(int argc, char *argv[]) {
    uhd::set_thread_priority_safe();

    //receive variables to be set by po
    std::string rx_args, rx_file, type, rx_ant, rx_subdev, ref;
    double rx_rate, rx_freq, end_freq, freq_step, rx_gain, rx_bw;
    size_t spb, total_num_samps;
    float settling;

    std::string pass, host;

    //setup the program options
    po::options_description desc("Allowed options");
    desc.add_options()
        ("help", "help message")
        ("args", po::value<std::string>(&rx_args)->default_value(""), "uhd receive device address args")
        ("file_prefix", po::value<std::string>(&rx_file)->default_value("usrp_samples2.dat"), "name of the file to write binary samples to")
        ("type", po::value<std::string>(&type)->default_value("short"), "sample type in file: double, float, or short")
        ("settling", po::value<float>(&settling)->default_value(float(0.2)), "settling time (seconds) before receiving")
        ("spb", po::value<size_t>(&spb)->default_value(64000), "samples per buffer")
        ("rate", po::value<double>(&rx_rate), "rate of receive incoming samples")
        ("freq", po::value<double>(&rx_freq), "receive RF center frequency in Hz")
        ("end_freq", po::value<double>(&end_freq), "RF end center frequency in Hz")
        ("freq_step", po::value<double>(&freq_step)->default_value(5000000), "RF frequency step in Hz")
        ("gain", po::value<double>(&rx_gain), "gain for the receive RF chain")
        ("ant", po::value<std::string>(&rx_ant), "receive antenna selection")
        ("subdev", po::value<std::string>(&rx_subdev), "receive subdevice specification")
        ("bw", po::value<double>(&rx_bw), "analog receive filter bandwidth in Hz")
        ("ref", po::value<std::string>(&ref)->default_value("internal"), "clock reference (internal, external, mimo)")
        ("nsamps", po::value<size_t>(&total_num_samps)->default_value(0), "total number of samples to receive")
        ("tx-pass", po::value<std::string>(&pass), "password of transmitter host")
        ("tx-host", po::value<std::string>(&host), "[username]@[hostname]")
    ;

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    //print the help message
    if (vm.count("help")){
        std::cout << boost::format("UHD RX samples to file %s") % desc << std::endl;
        return ~0;
    }

    //create a usrp device
    std::cout << std::endl;
    std::cout << boost::format("Creating the receive usrp device with: %s...") % rx_args << std::endl;
    uhd::usrp::multi_usrp::sptr rx_usrp = uhd::usrp::multi_usrp::make(rx_args);

    //Lock mboard clocks
    if (ref == "mimo") {
        uhd::clock_config_t clock_config;
        clock_config.ref_source = uhd::clock_config_t::REF_MIMO;
        clock_config.pps_source = uhd::clock_config_t::PPS_MIMO;
        rx_usrp->set_clock_config(clock_config);
    }
    else if (ref == "external") {
        rx_usrp->set_clock_config(uhd::clock_config_t::external());
        rx_usrp->set_time_unknown_pps(uhd::time_spec_t(0.0));
    }
    else if (ref == "internal") {
        rx_usrp->set_clock_config(uhd::clock_config_t::internal());
    }

    //always select the subdevice first, the channel mapping affects the other settings
    if (vm.count("subdev")) rx_usrp->set_rx_subdev_spec(rx_subdev);

    std::cout << boost::format("Using RX Device: %s") % rx_usrp->get_pp_string() << std::endl;

    const unsigned int rx_num_channels = rx_usrp->get_rx_num_channels();

    //set the receive sample rate
    if (not vm.count("rate")){
        std::cerr << "Please specify the sample rate with --rx-rate" << std::endl;
        return ~0;
    }
    std::cout << boost::format("Setting RX Rate: %f Msps...") % (rx_rate/1e6) << std::endl;
    for (unsigned int i = 0; i < rx_num_channels; ++i) {
        rx_usrp->set_rx_rate(rx_rate, i);
    }
    std::cout << boost::format("Actual RX Rate: %f Msps...") % (rx_usrp->get_rx_rate()/1e6) << std::endl << std::endl;

    //set the receive center frequency
    if (not vm.count("freq")){
        std::cerr << "Please specify the receive center frequency with --rx-freq" << std::endl;
        return ~0;
    }

    uhd::time_spec_t rx_cmd_time = rx_usrp->get_time_now() + uhd::time_spec_t(0.1);
    rx_usrp->set_command_time(rx_cmd_time);
    for (unsigned int i = 0; i < rx_num_channels; ++i) {
        std::cout << boost::format("Setting RX Freq: %f MHz...") % (rx_freq/1e6) << std::endl;
        uhd::tune_result_t r = rx_usrp->set_rx_freq(rx_freq, i);
        std::cout << r.to_pp_string() << std::endl;
        std::cout << boost::format("Actual RX Freq: %f MHz...") % (rx_usrp->get_rx_freq()/1e6) << std::endl << std::endl;
    }
    rx_usrp->clear_command_time();

    //set the receive rf gain
    if (vm.count("gain")){
        for (unsigned int i = 0; i < rx_num_channels; ++i) {
            std::cout << boost::format("Setting RX Gain: %f dB...") % rx_gain << std::endl;
            rx_usrp->set_rx_gain(rx_gain, i);
            std::cout << boost::format("Actual RX Gain: %f dB...") % rx_usrp->get_rx_gain() << std::endl << std::endl;
        }
    }

    //set the receive analog frontend filter bandwidth
    if (vm.count("bw")){
        for (unsigned int i = 0; i < rx_num_channels; ++i) {
            std::cout << boost::format("Setting RX Bandwidth: %f MHz...") % (rx_bw/1e6) << std::endl;
            rx_usrp->set_rx_bandwidth(rx_bw, i);
            std::cout << boost::format("Actual RX Bandwidth: %f MHz...") % (rx_usrp->get_rx_bandwidth()/1e6) << std::endl << std::endl;
        }
    }

    //set the receive antenna
    if (vm.count("ant")) {
        for (unsigned int i = 0; i < rx_num_channels; ++i) {
            rx_usrp->set_rx_antenna(rx_ant, i);
        }
    }

    boost::this_thread::sleep(boost::posix_time::milliseconds(100)); //allow for some setup time
    //Check Ref and LO Lock detect
    std::vector<std::string> rx_sensor_names;
    rx_sensor_names = rx_usrp->get_rx_sensor_names(0);
    if (std::find(rx_sensor_names.begin(), rx_sensor_names.end(), "lo_locked") != rx_sensor_names.end()) {
        uhd::sensor_value_t lo_locked = rx_usrp->get_rx_sensor("lo_locked",0);
        std::cout << boost::format("Checking RX: %s ...") % lo_locked.to_pp_string() << std::endl;
        UHD_ASSERT_THROW(lo_locked.to_bool());
    }

    rx_sensor_names = rx_usrp->get_mboard_sensor_names(0);
    if ((ref == "mimo") and (std::find(rx_sensor_names.begin(), rx_sensor_names.end(), "mimo_locked") != rx_sensor_names.end())) {
        uhd::sensor_value_t mimo_locked = rx_usrp->get_mboard_sensor("mimo_locked",0);
        std::cout << boost::format("Checking RX: %s ...") % mimo_locked.to_pp_string() << std::endl;
        UHD_ASSERT_THROW(mimo_locked.to_bool());
    }
    if ((ref == "external") and (std::find(rx_sensor_names.begin(), rx_sensor_names.end(), "ref_locked") != rx_sensor_names.end())) {
        uhd::sensor_value_t ref_locked = rx_usrp->get_mboard_sensor("ref_locked",0);
        std::cout << boost::format("Checking RX: %s ...") % ref_locked.to_pp_string() << std::endl;
        UHD_ASSERT_THROW(ref_locked.to_bool());
    }

    std::signal(SIGINT, &sig_int_handler);
    std::cout << "Press Ctrl + C to stop streaming..." << std::endl;

    // recv to file
    std::string file_name;
    do {
        file_name = rx_file + boost::to_string(rx_freq/1e6);
        if (type == "double") recv_to_file<std::complex<double> >(rx_usrp, "fc64", file_name, spb, settling, total_num_samps);
        else if (type =="float") recv_to_file<std::complex<float> >(rx_usrp, "fc32", file_name, spb, settling, total_num_samps);
        else if (type =="short") recv_to_file<std::complex<short> >(rx_usrp, "sc16", file_name, spb, settling, total_num_samps);
        else {
            throw std::runtime_error("Unknown type" + type);
            stop_signal_called = true;
            std::string killcmd = std::string() + "sshpass -p \"" + pass + "\" ssh -o StrictHostKeyChecking=no " + host + " \"echo " + pass + " | sudo -S killall -9 tx_samples_from_file_switch\"";
            std::system(killcmd.c_str());
        }
        std::string killcmd = std::string() + "sshpass -p \"" + pass + "\" ssh -o StrictHostKeyChecking=no " + host + " \"echo " + pass + " | sudo -S killall -2 tx_samples_from_file_switch\"";
        std::system(killcmd.c_str());
        if (rx_freq == end_freq) {
            stop_signal_called = true;
        }
        else {
            rx_freq += freq_step;
            uhd::time_spec_t rx_cmd_time = rx_usrp->get_time_now() + uhd::time_spec_t(0.1);
            rx_usrp->set_command_time(rx_cmd_time);
            for (unsigned int i = 0; i < rx_num_channels; ++i) {
                std::cout << boost::format("RX channel %u: Setting RX Freq: %f MHz...") % i % (rx_freq/1e6) << std::endl;
                rx_usrp->set_rx_freq(rx_freq, i);
                std::cout << boost::format("RX channel %u: Actual RX Freq: %f MHz...") % i % (rx_usrp->get_rx_freq()/1e6) << std::endl << std::endl;
            }
            rx_usrp->clear_command_time();
            boost::this_thread::sleep(boost::posix_time::milliseconds(100)); //allow for some setup time
        }
    } while(not stop_signal_called);

    //finished
    std::cout << std::endl << "Done!" << std::endl << std::endl;
    return 0;
}
