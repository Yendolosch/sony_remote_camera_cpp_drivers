#include "Sony_Remote_Camera.h"
#include "sony_remote_camera_implementation.h"
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <iostream>

using namespace std;
using namespace boost;
using namespace src;

const std::map<Shoot_Mode, std::string> Sony_Remote_Camera_Implementation::shoot_modes_availables
= { { shoot_mode_still, "still" }, { shoot_mode_movie, "movie" }, { shoot_mode_audio, "audio" }, { shoot_mode_intervalstill, "intervalstill" } };

std::shared_ptr<Sony_Remote_Camera_Interface> src::GetSonyRemoteCamera(string my_own_ip) {
	try {
		return std::shared_ptr<Sony_Remote_Camera_Interface>(new Sony_Remote_Camera_Implementation(my_own_ip));
	}
	catch (std::exception e) {
		cerr << e.what();
		return std::shared_ptr<Sony_Remote_Camera_Interface>(nullptr);
	}
}


Sony_Remote_Camera_Implementation::Sony_Remote_Camera_Implementation(string my_own_ip_address) 
	:data(nullptr),
	SSDP_Client(io_service,my_own_ip_address),
	socket_options(io_service),
	socket_liveview(io_service_liveview),
	tcp_resolver(io_service) {
	io_service.run();
}

Sony_Remote_Camera_Implementation::~Sony_Remote_Camera_Implementation() {
	liveview_thread.interrupt();
	socket_options.close();
	socket_liveview.close();
	io_service.stop();
	io_service_liveview.stop();
	liveview_thread.join();
	if(!got_it) free(data);
}

Sony_Capture_Error Sony_Remote_Camera_Implementation::Retrieve_Decription_File() {
	const string description_url(SSDP_Client.Get_Description_Url());
	size_t last_slash = description_url.rfind('/');
	size_t colon = description_url.rfind(':');
	string server = description_url.substr(7, colon-7);
	string port = description_url.substr(colon+1, last_slash - colon -1);
	string path = description_url.substr(last_slash);
	ostream request_stream(&tcp_request_options);
	request_stream << "GET " << path << " HTTP/1.1\r\n";
	request_stream << "Host: " << server << "\r\n";
	request_stream << "Accept: */*\r\n";
	request_stream << "\r\n";
	asio::ip::tcp::resolver::query query(server, port);
	asio::ip::tcp::resolver::iterator endpoint_iterator = tcp_resolver.resolve(query);
	asio::connect(socket_options, endpoint_iterator);
	boost::asio::async_write(socket_options, tcp_request_options,
		boost::bind(&Sony_Remote_Camera_Implementation::Handle_Write_HTTP_Request, this, false, boost::asio::placeholders::error));
	try {
		io_service.reset();
		io_service.run();
		Parse_Description();
		socket_options.close();
	} catch (ios_base::failure e) {
		cerr << e.what();
		return SC_ERROR;
	}
	return SC_NO_ERROR;
}


Sony_Capture_Error Sony_Remote_Camera_Implementation::Send_Options_Command(string json_request){
	ostream request_stream(&tcp_request_options);
	size_t last_slash = camera_service_url.rfind('/');
	size_t colon = camera_service_url.rfind(':');
	string server = camera_service_url.substr(7, colon - 7);
	string port = camera_service_url.substr(colon + 1, last_slash - colon - 1);
	string path = camera_service_url.substr(last_slash);
	request_stream << "POST " << path << "/camera HTTP/1.1\r\n";
	request_stream << "Accept: application/json-rpc\r\n";
	request_stream << "Content-Length: " << json_request.length() << "\r\n";
	request_stream << "Content-Type: application/json-rpc\r\n";
	request_stream << "Host:" << camera_service_url << "\r\n";
	request_stream << "\r\n";
	request_stream << json_request;
	if (!socket_options.is_open()) {
		asio::ip::tcp::resolver::query query(server, port);
		asio::ip::tcp::resolver::iterator endpoint_iterator = tcp_resolver.resolve(query);
		asio::connect(socket_options, endpoint_iterator);
	}
	boost::asio::async_write(socket_options, tcp_request_options,
		boost::bind(&Sony_Remote_Camera_Implementation::Handle_Write_HTTP_Request, this, false,
		boost::asio::placeholders::error));
	try {
		io_service.reset();
		io_service.run();
	}
	catch (ios_base::failure e) {
		cerr << e.what();
		return SC_ERROR;
	}
	return SC_NO_ERROR;
}


Sony_Capture_Error Sony_Remote_Camera_Implementation::Launch_Liveview(){
	//string json_request(Build_JSON_Command("startLiveview", {})); 
	string json_request("{\"method\": \"startLiveview\",\"params\" : [],\"id\" : 1,\"version\" : \"1.0\"}\r\n");
	if (auto err = Send_Options_Command(json_request)) return err;
	using boost::property_tree::ptree;
	ptree pt;
	string c = text_content.str();
	read_json(text_content, pt);
	auto v = pt.get_child("result").begin();
	liveview_url = v->second.data();
	liveview_thread = thread(bind(&Sony_Remote_Camera_Implementation::Read_Liveview_Continuously, this));
	return SC_NO_ERROR;
}

void Sony_Remote_Camera_Implementation::Read_Liveview_Continuously(){
	while (true){
		this_thread::interruption_point();
		ostream request_stream(&tcp_request_liveview);
		size_t last_slash = liveview_url.rfind('/');
		size_t colon = liveview_url.rfind(':');
		string server = liveview_url.substr(7, colon - 7);
		string port = liveview_url.substr(colon + 1, last_slash - colon - 1);
		string path = liveview_url.substr(last_slash);
		request_stream << "GET "<< path << " HTTP/1.1\r\n";
		request_stream << "Accept: image/jpeg\r\n";
		request_stream << "Host: " << server << "\r\n";
		request_stream << "\r\n";
		asio::ip::tcp::resolver::query query(server, port);
		boost::asio::ip::tcp::resolver resolver(io_service_liveview);
		asio::ip::tcp::resolver::iterator endpoint_iterator = resolver.resolve(query);
		asio::connect(socket_liveview,endpoint_iterator);
		boost::asio::async_write(socket_liveview, tcp_request_liveview,
			boost::bind(&Sony_Remote_Camera_Implementation::Handle_Write_HTTP_Request, this, true,
			boost::asio::placeholders::error));
		io_service_liveview.run();
		io_service_liveview.reset();
	}
}

void Sony_Remote_Camera_Implementation::Handle_Write_HTTP_Request(bool mode_liveview, const system::error_code& err) {
	asio::ip::tcp::socket& s = mode_liveview ? socket_liveview : socket_options;
	asio::streambuf& buf = mode_liveview ? tcp_response_liveview : tcp_response_options;
	if (!err) {
		// Read the response status line. The response_ streambuf will
		// automatically grow to accommodate the entire line. The growth may be
		// limited by passing a maximum size to the streambuf constructor.
		boost::asio::async_read_until(s, buf, "\r\n",
			boost::bind(&Sony_Remote_Camera_Implementation::Handle_Read_Status_Line, this, mode_liveview,
			boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
	} else {
		throw(ios_base::failure(err.message()));
	}
}

void Sony_Remote_Camera_Implementation::Handle_Read_Status_Line(bool mode_liveview, const system::error_code& err, const size_t bytes_transferred) {
	asio::ip::tcp::socket& s = mode_liveview ? socket_liveview : socket_options;
	asio::streambuf& buf = mode_liveview ? tcp_response_liveview : tcp_response_options;
	if (!err) {
		// Check that response is OK.
		std::istream response_stream(&buf);
		std::string http_version;
		response_stream >> http_version;
		unsigned int status_code;
		response_stream >> status_code;
		std::string status_message;
		std::getline(response_stream, status_message);
		if (!response_stream || http_version.substr(0, 5) != "HTTP/") {
			stringstream m;
			m << "Invalid response\n";
			m << http_version << '\n';
			throw(ios_base::failure(m.str()));
		}
		if (status_code != 200) {
			stringstream m;
			m << "Response returned with status code: ";
			m << status_code << '\n';
			throw(ios_base::failure(m.str()));
		}
		// Read the response headers, which are terminated by a blank line.
		boost::asio::async_read_until(s, buf, "\r\n\r\n",
			boost::bind(&Sony_Remote_Camera_Implementation::Handle_Read_Headers, this, mode_liveview,
			boost::asio::placeholders::error));
	}
	else {
		stringstream m;
		m << "Error: " << err << "\n";
		throw(ios_base::failure(m.str()));
	}
}

void Sony_Remote_Camera_Implementation::Handle_Read_Headers(bool mode_liveview, const system::error_code& err) {
	asio::ip::tcp::socket& s = mode_liveview ? socket_liveview : socket_options;
	asio::streambuf& buf = mode_liveview ? tcp_response_liveview : tcp_response_options;
	if (!err) {
		// Process the response headers.
		std::istream response_stream(&buf);
		std::string header;
		while (std::getline(response_stream, header) && header != "\r"){
			//std::cout << header << "\n";
		}
		if (mode_liveview){
			Read_Jpeg_Content();				
		} else {
			// Write whatever content we already have to output.
			if (buf.size() > 0) {
				//std::cout << &tcp_response_;
				text_content << &buf;
			}
			// Start reading remaining data until EOF.
			boost::asio::async_read(socket_options, tcp_response_options,
				boost::asio::transfer_at_least(1),
				boost::bind(&Sony_Remote_Camera_Implementation::Handle_Read_Text_Content, this,
				boost::asio::placeholders::error));
		}
	}
	else {
		stringstream m;
		m << "Error: " << err << "\n";
		throw(ios_base::failure(m.str()));
	}
}

void Sony_Remote_Camera_Implementation::Handle_Read_Text_Content(const boost::system::error_code& err) {
	if (!err) {
		// Write all of the data that has been read so far.
		text_content << &tcp_response_options;
		// Continue reading remaining data until EOF.
		boost::asio::async_read(socket_options, tcp_response_options,
			boost::asio::transfer_at_least(1),
			boost::bind(&Sony_Remote_Camera_Implementation::Handle_Read_Text_Content, this,
			boost::asio::placeholders::error));
	} else if (err != boost::asio::error::eof) {
		std::cout << "Error: " << err << "\n";
	}
}

void Sony_Remote_Camera_Implementation::Read_Jpeg_Content() {
	size_t n = tcp_response_liveview.size();
	vector<uint8_t> v;
	if (n < 12) {
		tcp_response_liveview.sgetn((char *)liveview_common_header, n);
		boost::asio::async_read(socket_liveview, asio::buffer(liveview_common_header + n, 12 - n),
			boost::bind(&Sony_Remote_Camera_Implementation::Handle_Read_Common_Header, this,
			boost::asio::placeholders::error, 0, 0));
	}
	else if (n < 12 + 128) {
	n -= 12;
	tcp_response_liveview.sgetn((char *)liveview_common_header, 12);
	tcp_response_liveview.sgetn((char *)liveview_payload_header, n);
	Handle_Read_Common_Header(system::error_code(), n, 0);
	} else {
		tcp_response_liveview.sgetn((char *)liveview_common_header, 12);
		tcp_response_liveview.sgetn((char *)liveview_payload_header, 128);
		n -= (128 + 12);
		tcp_response_liveview.sgetn((char *)&jpeg_image[0], n);
		Handle_Read_Common_Header(system::error_code(), 128, n);
	}
}

void Sony_Remote_Camera_Implementation::Handle_Read_Common_Header(const boost::system::error_code& err, size_t payload_bytes_already_read, size_t image_bytes_already_read) {
	static const vector<uint8_t> lv_on = { 0xff, 0x01 };
	if (mismatch(lv_on.begin(), lv_on.end(), liveview_common_header + 4).first != lv_on.end()) {
		return;
	}
	boost::asio::async_read(socket_liveview, asio::buffer(liveview_payload_header + payload_bytes_already_read, 128 - payload_bytes_already_read),
		boost::bind(&Sony_Remote_Camera_Implementation::Handle_Read_Payload_Header, this,
		boost::asio::placeholders::error, image_bytes_already_read));
}

void Sony_Remote_Camera_Implementation::Handle_Read_Payload_Header(const boost::system::error_code& err, size_t image_bytes_already_read) {
	size_t size = liveview_payload_header[6]<<0 | liveview_payload_header[5]<<8 | liveview_payload_header[4]<<24;
	size += 10;
	jpeg_image.resize(size);
	boost::asio::async_read(socket_liveview, asio::buffer(&jpeg_image[0] + image_bytes_already_read, size - image_bytes_already_read),
		boost::bind(&Sony_Remote_Camera_Implementation::Handle_Read_Image, this,
		boost::asio::placeholders::error));
}
void Sony_Remote_Camera_Implementation::Handle_Read_Image(const boost::system::error_code& err) {
	//cv::Mat imgbuf(image,false);
	//cv::Mat m(cv::imdecode(imgbuf, cv::IMREAD_COLOR)); 
	int ts, fn;
	ts = 0;
	fn = 0;
	static const vector<uint8_t> start_marker = { 0xff, 0xd8 };
	static const vector<uint8_t> end_marker = { 0xff, 0xd9 };
	auto start = search(jpeg_image.begin(), jpeg_image.end(), start_marker.begin(), start_marker.end());
	auto end = search(jpeg_image.begin(), jpeg_image.end(), end_marker.begin(), end_marker.end());
	size_t a = start - jpeg_image.begin();
	size_t b = end - jpeg_image.begin();
	//ofstream image_file("image.jpg", ios::out | ios::binary | ios::trunc);
	//image_file.write((char*)&(*start), jpeg_image.end() - start);
	//image_file.close();
	boost::asio::async_read(socket_liveview, asio::buffer(liveview_common_header, 12),
		boost::bind(&Sony_Remote_Camera_Implementation::Handle_Read_Common_Header, this,
		boost::asio::placeholders::error, 0, 0));
	auto ita = jpeg_image.begin();
	auto itb = jpeg_image.end();
	if (end != jpeg_image.end()) end+=2; // we want to copy end if we found it
	uint8_t* new_data = (uint8_t*)malloc(end - start);
	copy(start, end, new_data);
	liveview_mutex.lock();
	file_size = end - start;
	uint8_t* old_data = data;
	data = new_data;
	timestamp = ts;
	frame_number = fn;
	bool free_old_data = !got_it;
	got_it = false;
	liveview_mutex.unlock();
	if (free_old_data) {
		free(old_data);
	}
}

void Sony_Remote_Camera_Implementation::Parse_Description() {
	using boost::property_tree::ptree;
	ptree pt;
	read_xml(text_content, pt);
	liveview_url = pt.get<string>("root.device.av:X_ScalarWebAPI_DeviceInfo.av:X_ScalarWebAPI_ImagingDevice.av:X_ScalarWebAPI_LiveView_URL");
	for (ptree::value_type &v : pt.get_child("root.device.av:X_ScalarWebAPI_DeviceInfo.av:X_ScalarWebAPI_ServiceList")){
		string service = v.second.get<string>("av:X_ScalarWebAPI_ServiceType");
		if (service == "camera")
			camera_service_url = v.second.get<string>("av:X_ScalarWebAPI_ActionList_URL");
	}
}

Sony_Capture_Error
Sony_Remote_Camera_Implementation::Get_Last_JPeg_Image(uint8_t*& data, size_t& size, int& frame_number, int& timestamp){
	//return jpeg_image;
	liveview_mutex.lock();
	auto err = SC_NO_ERROR;
	if (this->data && !got_it) {
		data = this->data;
		size = file_size;
		frame_number = this->frame_number;
		timestamp = this->timestamp;
		got_it = true;
	} else {
		err = SC_NO_NEW_DATA_AVAILABLE;
	}
	liveview_mutex.unlock();
	return err;
}

Sony_Capture_Error
Sony_Remote_Camera_Implementation::Set_Shoot_Mode(Shoot_Mode mode) {
	string command = Build_JSON_Command("setShootMode", { shoot_modes_availables.at(mode) });
	//string command = "{\n\"method\": \"setShootMode\",\n\"params\" : [\"movie\"],\n\"id\" : 1,\n\"version\" : \"1.0\"\n}";
	if (auto err = Send_Options_Command(command)) return err;
	return SC_NO_ERROR;
	//else return SC_WRONG_PARAMETER;
}

string Sony_Remote_Camera_Implementation::Build_JSON_Command(const string& method, const vector<string>& params){
	//using boost::property_tree::ptree;
	//ptree pt;
	//pt.put("method", "setShootMode");
	//ptree child_params; 
	//for (auto s : params) {
	//	ptree p;
	//	p.put("", s);
	//	child_params.push_back(make_pair("", p));
	//}/*
	//if (params.empty()) {
	//	ptree p;
	//	p.put("", "");
	//	child_params.push_back(make_pair("", p));
	//}*/
	//pt.add_child("params",child_params);
	//pt.put<int>("id",++current_json_id);
	//pt.put("version", "1.0");
	//stringstream ss;
	//property_tree::write_json(ss,pt);
	//string command = ss.str();
	//replace(command.begin(), command.end(), '\n', ' ');
	stringstream command;
	command << "{\n";
	command << "\t\"method\": \"" << method << "\"\n";
	command << "\t\"params\": [\n";
	for (auto s : params)
		command << "\t\t\"" << s << "\",\n";
	command.seekp(int(command.tellp()) - 2);
	command << "\n]\n";
	command << "\t\"id\": " << ++current_json_id << "\n";
	command << "\t\"version\": \"1.0\"\n";
	command << "}\n";
	return command.str();
}