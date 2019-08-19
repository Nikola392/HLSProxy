
#include "HLSProxy.h"
#include <ws2tcpip.h>
#include "log.h"
#include "CDNConnection.h"

#include <stdlib.h> // atoi
#include <sstream>

#if defined(WIN32)
// pthreadVC2.dll		- built with MSVC compiler using C setjmp/longjmp
// pthreadVCE2.dll		- built with MSVC++ compiler using C++ EH
// pthreadVSE2.dll		- built with MSVC compiler using SEH
#	pragma comment(lib,"pthreadVC2.lib")
#endif


//   https://bitdash-a.akamaihd.net/content/MI201109210084_1/m3u8s/f08e80da-bf1d-4e3d-8899f0f6155f6efa.m3u8
// working ones:
//   https://bitdash-a.akamaihd.net/content/MI201109210084_1/m3u8s/f08e80da-bf1d-4e3d-8899-f0f6155f6efa.m3u8
//   https://bitdash-a.akamaihd.net/content/sintel/hls/playlist.m3u8
//   https://mnmedias.api.telequebec.tv/m3u8/29880.m3u8
//   http://184.72.239.149/vod/smil:BigBuckBunny.smil/playlist.m3u8
//   http://www.streambox.fr/playlists/test_001/stream.m3u8

// http://127.0.0.1:8080/1~443~bitdash-a.akamaihd.net/content/MI201109210084_1/m3u8s/f08e80da-bf1d-4e3d-8899f0f6155f6efa.m3u8

int main(int argc, char **argv)
{
	std::string listen_host;
	uint32_t    listen_port;

	if(argc < 3) {
		LOG << "Usage: hls_proxy address_to_bind_to listen_port";
		return -1;
	}

	// TODO: error checking
 	listen_host = argv[1];
	listen_port = atoi(argv[2]);

	try {
		HLSProxyServer server(listen_host, listen_port);
		server.run_forever();
	} catch(BaseError err) {
		LOGE << "Exception causing server exit: " << err.reason;
	}
    return 0;
}

void * client_connected_proxy(void * client) {
	HLSClient * self = (HLSClient *) client;
	try {
		self->run_player_request_parsing();
	} catch(BaseError err) {
		LOGE << "run_player_request_parsing::Exception: " << err.reason;
	}
	self->cleanup();
	return NULL;
}

//HLSProxyServer::HLSProxyServer(std::string default_cdn_host, uint32_t default_cdn_port, uint32_t listen_port) 
//	: _default_host(default_cdn_host),
//	  _default_port(default_cdn_port),
//	  _listen_port (listen_port)
//{
//	// ...
//	_listen_socket       = INVALID_SOCKET;
//	_listen_backlog_size = 5;
//}

HLSProxyServer::HLSProxyServer(std::string listen_host, uint32_t listen_port) :
	_listen_host(listen_host),
	_listen_port(listen_port)
{
	_listen_socket       = INVALID_SOCKET;
	_listen_backlog_size = 5;
	_recvbuf_size        = 1024 * 1024;
	_sendbuf_size        = 1024 * 1024;
}

HLSProxyServer::~HLSProxyServer() {
	// TODO: ...
	closesocket(_listen_socket);
}

void HLSProxyServer::run_forever() {
	// Setup server and accept loop
#ifdef WIN32 
	WSADATA returned_version;
	int status = WSAStartup(MAKEWORD(2, 2), &returned_version);
	if(status != NO_ERROR) {
		throw ErrorCritical("WSAStartup() error");
	}
	if(LOBYTE(returned_version.wVersion) != 2 || HIBYTE(returned_version.wVersion) != 2) {
		throw ErrorCritical("Winsock.dll error");
	}
#endif

	_listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if(_listen_socket == INVALID_SOCKET) {
		throw ErrorCritical("listen socket alloc error");
	}
	sockaddr_in server;
	server.sin_family      = AF_INET;
	server.sin_addr.s_addr = INADDR_ANY;
	server.sin_port        = htons(_listen_port);

	if(bind(_listen_socket, (SOCKADDR *) &server, sizeof(server)) < 0) {
		throw ErrorCritical("bind() error");
	}

	if(listen(_listen_socket, _listen_backlog_size) < 0) {
		throw ErrorCritical("listen() error");
	}

	while(true) {
		int status;
		sockaddr_in client_address;
		int         address_size  = sizeof(client_address);
		SOCKET      client_socket = accept(_listen_socket, (struct sockaddr *) &client_address, &address_size);
#ifdef WIN32
		if(client_socket == INVALID_SOCKET) {
#else
		if(client_socket < 0) {
#endif
			throw ErrorCritical("listen() error");
		}
		status = setsockopt(client_socket, SOL_SOCKET, SO_RCVBUF, (char *) &_recvbuf_size, sizeof(int));
		if(status == SOCKET_ERROR) {
			throw ErrorCritical("setsockopt(SO_RCVBUF) error");
		}
		status = setsockopt(client_socket, SOL_SOCKET, SO_SNDBUF, (char *) &_sendbuf_size, sizeof(int));
		if(status == SOCKET_ERROR) {
			throw ErrorCritical("setsockopt(SO_RCVBUF) error");
		}
		// create client context and spawn new processing thread
		HLSClient * client = new HLSClient(this, client_socket, client_address, address_size);
		status             = pthread_create(&client->_thread_handle, NULL, client_connected_proxy, client);
		if(status != 0) {
			throw ErrorCritical("accept pthread_create() error");
		}
		_client_list.push_back(client);
	}
}


HLSClient::HLSClient(HLSProxyServer * server, SOCKET client_socket, sockaddr_in client_address, int address_size) :
	_socket(client_socket),
	_server(server)
{
	char buffer[INET_ADDRSTRLEN];
	const char * ret = inet_ntop(AF_INET, &(client_address.sin_addr), buffer, INET_ADDRSTRLEN);
	if(ret == NULL) {
		throw ErrorCritical("inet_ntop() error");
	}
	_address = buffer; // store null terminated string
}


// ----------------
// HLSClient::run_player_request_parsing()
// 
//   CDN destination encoding format:
//     {1,0}~{port}~{domain}
//   Examples: assume server is serving on 127.0.0.1 on port 8080
//     https://bitdash-a.akamaihd.net/                                 >> http://127.0.0.1:8080/1~443~bitdash-a.akamaihd.net
//     https://mnmedias.api.telequebec.tv/m3u8/29880.m3u8              >> http://127.0.0.1:8080/1~443~mnmedias.api.telequebec.tv/m3u8/29880.m3u8
//     http://184.72.239.149/vod/smil:BigBuckBunny.smil/playlist.m3u8  >> http://127.0.0.1:8080/0~80~184.72.239.149/vod/smil:BigBuckBunny.smil/playlist.m3u8
//     http://www.streambox.fr/playlists/test_001/stream.m3u8          >> http://127.0.0.1:8080/0~80~www.streambox.fr/playlists/test_001/stream.m3u8
// 1) receive request
// 2) modify HTTP initial line and headers.
//      example request:
/*
GET /1~443~bitdash-a.akamaihd.net/content/MI201109210084_1/m3u8s/f08e80da-bf1d-4e3d-8899-f0f6155f6efa.m3u8 HTTP/1.0
Host: 127.0.0.1:8080

*/
//      transforms into:
/*
GET /content/MI201109210084_1/m3u8s/f08e80da-bf1d-4e3d-8899-f0f6155f6efa.m3u8 HTTP/1.0
Host: bitdash-a.akamaihd.net:443

*/
//      and CDN destination is identified as `bitdash-a.akamaihd.net:80`
// 3) connect to appropriate CDN destination
// 4) stream request to CDN destination
// 5) receive response from CDN and stream back to client. 
//   Send back in chunks of entire lines to allow URL rewrite logic to take effect
// 6) detect if response body is play-list and if so apply URL rewrite
//   Actual domain and port will be encoded and prepended as first param to URL path
//   example serving on 127.0.0.1:8080 :
//     https://bitdash-a.akamaihd.net/content/MI201109210084_1/m3u8s/f08e80da-bf1d-4e3d-8899-f0f6155f6efa.m3u8
//       transforms into:
//     http://127.0.0.1:8080/1~443~bitdash-a.akamaihd.net/content/MI201109210084_1/m3u8s/f08e80da-bf1d-4e3d-8899-f0f6155f6efa.m3u8
void HLSClient::run_player_request_parsing() {
	// reading request
	DataBuffer        request_buffer(8192);
	HTTPParser        request_parser(true);
	while(request_parser._headers_complete == false) { // TODO: can we move to next stage when URL is received ?
		request_buffer.read_next_chunk(_socket);
		const char * data         = request_buffer._storage + request_buffer._position;
		size_t    data_size       = request_buffer._bytes_written - request_buffer._position;
		size_t bytes_parsed       = request_parser.process_data(data, data_size);
		request_buffer._position += bytes_parsed;
	}
	// modify HTTP initial line and headers
	URLTransformation  modified(request_parser._url);
	std::ostringstream cdn_request;
	cdn_request << request_parser._method << " " << modified.result_url << " " << request_parser._http_version << "\r\n";
	// Place modified Host header
	cdn_request << "Host: " << modified.cdn_domain << ":" << modified.cdn_port << "\r\n";
	// Include all headers except of Host header
	std::deque<HTTPHeader *>::iterator iterator = request_parser._headers.begin();
	while(iterator != request_parser._headers.end()) {
		HTTPHeader * header = *iterator;
		iterator++;
		//if(0 == header->field.compare("Host")) {
#ifdef WIN32
		if(0 == _stricmp(header->field.c_str(), "host")) {
#else
		if(0 == strcasecmp(header->field.c_str(), "host")) {
#endif
			continue;
		}
		cdn_request << header->field << ": " << header->value << "\r\n";
	}
	// Place headers terminator
	cdn_request << "\r\n";
	_modified_request = cdn_request.str();
	// connect to appropriate CDN destination. Use ssl if modified.use_ssl == true.
	if(modified.use_ssl) {
		_cdn_connection = new CDNConnectionSSL();
	} else {
		_cdn_connection = new CDNConnection();
	}
	_cdn_connection->connect(modified.cdn_domain, modified.cdn_port);
	// send HTTP headers
	_cdn_connection->send((char*) _modified_request.c_str(), _modified_request.size());
	// send partial body from request_parser._body
	DataBuffer & transfer_buffer = request_parser._body;
	int body_bytes = transfer_buffer._bytes_written - transfer_buffer._position;
	if(body_bytes > 0) {
		_cdn_connection->send(transfer_buffer._storage + transfer_buffer._position, body_bytes);
		transfer_buffer._position += body_bytes;
		transfer_buffer.clear();
	}
	// if request_parser._body_complete == true call run_cdn_response_parsing else start new thread for run_cdn_response_parsing
	if(request_parser._body_complete == true) {
		run_cdn_response_parsing();
		return;
	}
	// start new thread for run_cdn_response_parsing()
	pthread_t thread_handle;
	int status = pthread_create(&thread_handle, NULL, run_cdn_response_parsing_proxy, this);
	if(status != 0) {
		throw ErrorCritical("accept pthread_create() error");
	}

	// stream body until request_parser._body_complete == true
	//while(request_parser._body_complete == false) {
	while(true) {
		transfer_buffer.clear();
		transfer_buffer.read_next_chunk(_socket); // this will raise socket broken
		_cdn_connection->send(transfer_buffer._storage, transfer_buffer._bytes_written);
	}

// 	void * retval = NULL;
// 	pthread_join(thread_handle, &retval);
}

void HLSClient::run_cdn_response_parsing() {
	// body of the request is either all sent or is streamed in other thread.
	// we now read _cdn_connection and parse response

	DataBuffer        response_buffer(8192);
	HTTPParser        response_parser(false);
	while(response_parser._headers_complete == false) {
		_cdn_connection->read_next_chunk(&response_buffer);
		const char * data         = response_buffer._storage + response_buffer._position;
		size_t    data_size       = response_buffer._bytes_written - response_buffer._position;
		size_t bytes_parsed       = response_parser.process_data(data, data_size);
		response_buffer._position += bytes_parsed;
		if(response_buffer._position == response_buffer._bytes_written) {
			response_buffer.clear();
		}
	}
	// prepare response to player based on info from CDN
	std::ostringstream cdn_response;
	cdn_response << response_parser._http_version << " " << response_parser._response_code << " " << http_status_str((enum http_status) response_parser._response_code) << "\r\n";
	std::deque<HTTPHeader *>::iterator iterator = response_parser._headers.begin();
	while(iterator != response_parser._headers.end()) {
		HTTPHeader * header = *iterator;
		iterator++;
		cdn_response << header->field << ": " << header->value << "\r\n";
	}
	// Place headers terminator
	cdn_response << "\r\n";
	// detect if response is a playlist. 
	if(true) {
		// If not then just pass response body unchanged to player.
		std::string response = cdn_response.str();
		send_to_player_nossl((char *) response.c_str(), response.size());
		// send parsed body so far
		// // //char * data          = response_buffer._storage + response_buffer._position;
		// // //size_t    data_size  = response_buffer._bytes_written - response_buffer._position;
		// // //if(data_size > 0) {
		// // //	send_to_player_nossl(data, data_size);
		// // //}
		// // //response_buffer.clear();
		if(response_parser._body._bytes_written > 0) {
			send_to_player_nossl(response_parser._body._storage, response_parser._body._bytes_written);
			response_parser._body.clear();
		}

		// stream body
		while(response_parser._body_complete == false) {
			_cdn_connection->read_next_chunk(&response_buffer);
			const char * input_data   = response_buffer._storage + response_buffer._position;
			size_t input_data_size    = response_buffer._bytes_written - response_buffer._position;
			size_t bytes_parsed       = response_parser.process_data(input_data, input_data_size);
			response_buffer._position += bytes_parsed;
			if(response_buffer._position == response_buffer._bytes_written) {
				response_buffer.clear();
			}
			DataBuffer & output       = response_parser._body;
			if(output._bytes_written > 0) {
				send_to_player_nossl(output._storage, output._bytes_written);
				output.clear();
			}
		}
		LOG << "Transfer complete";
		closesocket(_socket);
		_cdn_connection->close();
		return;
	}
	//   Content-Type: header
	//   handle Content-Length: header
	// If playlist then read entire line, transform URLs, send to player

}

void * HLSClient::run_cdn_response_parsing_proxy(void * client) {
	HLSClient * self = (HLSClient *) client;
	try {
		self->run_cdn_response_parsing();
	} catch(BaseError err) {
		LOGE << "run_cdn_response_parsing::Exception: " << err.reason;
	}
	self->cleanup();
	return NULL;
}

void HLSClient::send_to_player_nossl(char * data, int data_size) {
	while(data_size > 0) {
		int bytes_sent = send(_socket, data, data_size, 0);
		if(SOCKET_ERROR == bytes_sent) {
			LOGE << "DataBuffer::send_no_ssl send() error=" << WSAGetLastError();
			throw EventConnectionBroken();
		}
		data_size     -= bytes_sent;
	}
}

void HLSClient::cleanup() {
	closesocket(_socket);
	if(_cdn_connection != NULL) {
		_cdn_connection->close();
	}
}





DataBuffer::DataBuffer(int initial_size) {
	_bytes_allocated = initial_size;
	_storage         = (char *) malloc(initial_size);
	_position        = 0;
	_bytes_written   = 0;
	_CHUNK_SIZE_     = 8192;
}

DataBuffer::~DataBuffer() {
	free(_storage);
}

void DataBuffer::read_next_chunk(SOCKET socket) {
	realloc_capacity(_CHUNK_SIZE_);
	int count = recv(socket, _storage + _position, _CHUNK_SIZE_, 0);
	if(count <= 0) {
		// socket is broken
		LOGE << "DataBuffer::read_next_chunk returned <= 0";
		throw EventConnectionBroken();
	}
	_bytes_written += count;
}

void DataBuffer::realloc_capacity(int amount_needed) {
	int existing_space = _bytes_allocated - _position;
	if(existing_space < amount_needed) {
		int space_to_allocate = _position + amount_needed + _CHUNK_SIZE_;
		_storage              = (char *) realloc(_storage, space_to_allocate);
		if(_storage == NULL) {
			LOGE << "DataBuffer::realloc_capacity realloc() error amount_needed=" << amount_needed << " space_to_allocate=" << space_to_allocate;
			throw ErrorOutOfMemory("DataBuffer::realloc()");
		}
		_bytes_allocated = space_to_allocate;
	}
}

void DataBuffer::clear() {
	_bytes_written = 0;
	_position      = 0;
}

DataBuffer::DataBuffer() {
	_bytes_allocated = 0;
	_storage         = NULL;
	_position        = 0;
	_bytes_written   = 0;
	_CHUNK_SIZE_     = 8192;
}





// Callbacks must return 0 on success. Returning a non-zero value indicates error to the parser, making it exit immediately.
int HTTPParser::request_on_message_begin(http_parser * parser) {
	HTTPParser * self = (HTTPParser *) (parser->data);
	printf("message begin\n");
	return 0;
}

int HTTPParser::request_on_headers_complete(http_parser * parser) {
	HTTPParser * self = (HTTPParser *) (parser->data);
	self->_headers_complete  = true;
	self->_method            = http_method_str((enum http_method) self->_parser.method);
	// self->_response_status   = http_status_str(self->_parser.status_code);
	self->_response_code     = self->_parser.status_code;
	std::ostringstream  version;
	version << "HTTP/" << self->_parser.http_major << "." << self->_parser.http_minor;
	self->_http_version      = version.str();
	printf("headers complete\n");
	return 0;
}

int HTTPParser::request_on_message_complete(http_parser * parser) {
	HTTPParser * self = (HTTPParser *) (parser->data);
	printf("message complete\n");
	return 0;
}

int HTTPParser::request_on_url(http_parser * parser, const char* at, size_t length) {
	printf("Url: %.*s\n", (int) length, at);
	HTTPParser * self = (HTTPParser *) (parser->data);
	self->_url += std::string(at, length);
	return 0;
}

int HTTPParser::request_on_header_field(http_parser * parser, const char* at, size_t length) {
	printf("Header field: %.*s\n", (int) length, at);
	HTTPParser * self               = (HTTPParser *) (parser->data);
	if(self->_header_parsing_state == HTTPHeader::REPORTED_NONE || self->_header_parsing_state == HTTPHeader::REPORTED_VALUE) {
		self->_last_reported_field  = std::string(at, length);
	} else {
		// HTTPHeader::REPORTED_FIELD
		self->_last_reported_field += std::string(at, length);
	}
	self->_header_parsing_state     = HTTPHeader::REPORTED_FIELD;
	return 0;
}

int HTTPParser::request_on_header_value(http_parser * parser, const char* at, size_t length) {
	printf("Header value: %.*s\n", (int) length, at);
	HTTPParser * self = (HTTPParser *) (parser->data);
	if(self->_header_parsing_state == HTTPHeader::REPORTED_VALUE) {
		if(self->_headers.size() > 0) {
			HTTPHeader * header = self->_headers.back();
			header->value      += std::string(at, length);
		}
	} else if(self->_header_parsing_state == HTTPHeader::REPORTED_FIELD) {
		HTTPHeader * header = new HTTPHeader();
		header->field       = self->_last_reported_field;
		header->value       = std::string(at, length);
		self->_headers.push_back(header);
	}
	self->_header_parsing_state = HTTPHeader::REPORTED_VALUE;
	return 0;
}

int HTTPParser::request_on_body(http_parser * parser, const char* at, size_t length) {
// 	printf("Body: %.*s\n", (int) length, at);
	HTTPParser * self     = (HTTPParser *) (parser->data);
	DataBuffer & data     = self->_body;
	data.realloc_capacity(data._bytes_written - data._position + length);
	memcpy(data._storage + data._bytes_written, at, length);
	data._bytes_written  += length;
	self->_body_complete  = (http_body_is_final(& self->_parser) == 1);
	return 0;
}

HTTPParser::HTTPParser(bool request) :
	_header_parsing_state(HTTPHeader::HeaderParsingState::REPORTED_NONE),
	_headers_complete(false),
	_body_complete(false),
	_body(4096),
	_response_code(0)
{
	memset(&_settings, 0, sizeof(_settings));

	_settings.on_message_begin    = & (HTTPParser::request_on_message_begin);
	_settings.on_url              = & (HTTPParser::request_on_url);
	_settings.on_header_field     = & (HTTPParser::request_on_header_field);
	_settings.on_header_value     = & (HTTPParser::request_on_header_value);
	_settings.on_headers_complete = & (HTTPParser::request_on_headers_complete);
	_settings.on_body             = & (HTTPParser::request_on_body);
	_settings.on_message_complete = & (HTTPParser::request_on_message_complete);

	http_parser_init(&_parser, request ? HTTP_REQUEST : HTTP_RESPONSE);
	_parser.data = this;
}

size_t HTTPParser::process_data(const char * data, size_t data_size) {
	return http_parser_execute(&_parser, &_settings, data, data_size);
}

URLTransformation::URLTransformation(std::string input_url) {
	// Example:
	// "/1~443~bitdash-a.akamaihd.net/content/MI201109210084_1/m3u8s/f08e80da-bf1d-4e3d-8899-f0f6155f6efa.m3u8"
	if(input_url.size() < 7) {
		LOGE << "short input to URLTransformation: " << input_url;
		throw ErrorCritical("short input to URLTransformation");
	}
	if(input_url.at(0) != '/') {
		LOGE << "bad input to URLTransformation: " << input_url;
		throw ErrorCritical("bad input to URLTransformation");
	}
	size_t start_of_cdn_url = input_url.find('/', 1);
	if(start_of_cdn_url == std::string::npos) {
		LOGE << "bad input to URLTransformation: " << input_url;
		throw ErrorCritical("bad input to URLTransformation. second / missing");
	}
	size_t first_separator  = input_url.find('~', 1);
	if(first_separator == std::string::npos || first_separator > start_of_cdn_url) {
		LOGE << "bad input to URLTransformation: " << input_url;
		throw ErrorCritical("bad input to URLTransformation. first separator missing");
	}
	size_t second_separator = input_url.find('~', first_separator + 1);
	if(second_separator == std::string::npos || second_separator > start_of_cdn_url) {
		LOGE << "bad input to URLTransformation: " << input_url;
		throw ErrorCritical("bad input to URLTransformation. second separator missing");
	}
	std::string ssl_present = input_url.substr(1, first_separator - 1);
	std::string port_string = input_url.substr(first_separator  + 1, second_separator - first_separator  - 1);
	cdn_domain              = input_url.substr(second_separator + 1, start_of_cdn_url - second_separator - 1);
	result_url              = input_url.substr(start_of_cdn_url, input_url.size() - start_of_cdn_url); // we want to keep '/' at start_of_cdn_url
	if(ssl_present.size() != 1 || (ssl_present.at(0) != '1' && ssl_present.at(0) != '0')) {
		LOGE << "bad input to URLTransformation: " << input_url;
		throw ErrorCritical("bad input to URLTransformation. ssl_present element must be '1' or '0'");
	}
	use_ssl                 = (ssl_present.at(0) == '1');
	cdn_port                = atoi(port_string.c_str());
}


