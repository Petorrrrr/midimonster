#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>

#include "libmmbackend.h"
#include "rtpmidi.h"

#define BACKEND_NAME "rtpmidi"

static struct /*_rtpmidi_global*/ {
	int mdns_fd;
	char* mdns_name;
	uint8_t detect;
} cfg = {
	.mdns_fd = -1,
	.mdns_name = NULL,
	.detect = 0
};

int init(){
	backend rtpmidi = {
		.name = BACKEND_NAME,
		.conf = rtpmidi_configure,
		.create = rtpmidi_instance,
		.conf_instance = rtpmidi_configure_instance,
		.channel = rtpmidi_channel,
		.handle = rtpmidi_set,
		.process = rtpmidi_handle,
		.start = rtpmidi_start,
		.shutdown = rtpmidi_shutdown
	};

	if(sizeof(rtpmidi_channel_ident) != sizeof(uint64_t)){
		fprintf(stderr, "rtpmidi channel identification union out of bounds\n");
		return 1;
	}

	if(mm_backend_register(rtpmidi)){
		fprintf(stderr, "Failed to register rtpmidi backend\n");
		return 1;
	}

	return 0;
}

static int rtpmidi_configure(char* option, char* value){
	char* host = NULL, *port = NULL;

	if(!strcmp(option, "mdns-name")){
		if(cfg.mdns_name){
			fprintf(stderr, "Duplicate mdns-name assignment\n");
			return 1;
		}

		cfg.mdns_name = strdup(value);
		if(!cfg.mdns_name){
			fprintf(stderr, "Failed to allocate memory\n");
			return 1;
		}
		return 0;
	}
	else if(!strcmp(option, "mdns-bind")){
		if(cfg.mdns_fd >= 0){
			fprintf(stderr, "Only one mDNS discovery bind is supported\n");
			return 1;
		}

		mmbackend_parse_hostspec(value, &host, &port);

		if(!host){
			fprintf(stderr, "Not a valid mDNS bind address: %s\n", value);
			return 1;
		}

		cfg.mdns_fd = mmbackend_socket(host, (port ? port : RTPMIDI_MDNS_PORT), SOCK_DGRAM, 1, 1);
		if(cfg.mdns_fd < 0){
			fprintf(stderr, "Failed to bind mDNS interface: %s\n", value);
			return 1;
		}
		return 0;
	}
	else if(!strcmp(option, "detect")){
		cfg.detect = 0;
		if(!strcmp(value, "on")){
			cfg.detect = 1;
		}
		return 0;
	}

	fprintf(stderr, "Unknown rtpmidi backend option %s\n", option);
	return 1;
}

static int rtpmidi_configure_instance(instance* inst, char* option, char* value){
	rtpmidi_instance_data* data = (rtpmidi_instance_data*) inst->impl;

	if(!strcmp(option, "mode")){
		if(!strcmp(value, "direct")){
			data->mode = direct;
			return 0;
		}
		else if(!strcmp(value, "apple")){
			data->mode = apple;
			return 0;
		}
		fprintf(stderr, "Unknown rtpmidi instance mode %s for instance %s\n", value, inst->name);
		return 1;
	}
	else if(!strcmp(option, "ssrc")){
		data->ssrc = strtoul(value, NULL, 0);
		if(!data->ssrc){
			fprintf(stderr, "Random SSRC will be generated for rtpmidi instance %s\n", inst->name);
		}
		return 0;
	}
	else if(!strcmp(option, "bind")){
		//TODO set the bind host
	}
	else if(!strcmp(option, "learn")){
		if(data->mode != direct){
			fprintf(stderr, "The rtpmidi 'learn' option is only valid for direct mode instances\n");
			return 1;
		}
		data->learn_peers = 0;
		if(!strcmp(value, "true")){
			data->learn_peers = 1;
		}
		return 0;
	}
	else if(!strcmp(option, "peer")){
		if(data->mode != direct){
			fprintf(stderr, "The rtpmidi 'peer' option is only valid for direct mode instances\n");
			return 1;
		}

		//TODO add peer
		return 0;
	}
	else if(!strcmp(option, "session")){
		if(data->mode != apple){
			fprintf(stderr, "The rtpmidi 'session' option is only valid for apple mode instances\n");
			return 1;
		}
		free(data->session_name);
		data->session_name = strdup(value);
		if(!data->session_name){
			fprintf(stderr, "Failed to allocate memory\n");
			return 1;
		}
		return 0;
	}
	else if(!strcmp(option, "invite")){
		if(data->mode != apple){
			fprintf(stderr, "The rtpmidi 'invite' option is only valid for apple mode instances\n");
			return 1;
		}
		free(data->invite_peers);
		data->invite_peers = strdup(value);
		if(!data->invite_peers){
			fprintf(stderr, "Failed to allocate memory\n");
			return 1;
		}
		return 0;
	}
	else if(!strcmp(option, "join")){
		if(data->mode != apple){
			fprintf(stderr, "The rtpmidi 'join' option is only valid for apple mode instances\n");
			return 1;
		}
		free(data->invite_accept);
		data->invite_accept = strdup(value);
		if(!data->invite_accept){
			fprintf(stderr, "Failed to allocate memory\n");
			return 1;
		}
		return 0;
	}

	fprintf(stderr, "Unknown rtpmidi instance option %s\n", option);
	return 1;
}

static instance* rtpmidi_instance(){
	rtpmidi_instance_data* data = NULL;
	instance* inst = mm_instance();

	if(!inst){
		return NULL;
	}

	data = calloc(1, sizeof(rtpmidi_instance_data));
	if(!data){
		fprintf(stderr, "Failed to allocate memory\n");
		return NULL;
	}

	inst->impl = data;
	return inst;
}

static channel* rtpmidi_channel(instance* inst, char* spec, uint8_t flags){
	char* next_token = spec;
	rtpmidi_channel_ident ident = {
		.label = 0
	};

	if(!strncmp(spec, "ch", 2)){
		next_token += 2;
		if(!strncmp(spec, "channel", 7)){
			next_token = spec + 7;
		}
	}
	else{
		fprintf(stderr, "Invalid rtpmidi channel specification %s\n", spec);
		return NULL;
	}

	ident.fields.channel = strtoul(next_token, &next_token, 10);
	if(ident.fields.channel > 15){
		fprintf(stderr, "rtpmidi channel out of range in channel spec %s\n", spec);
		return NULL;
	}

	if(*next_token != '.'){
		fprintf(stderr, "rtpmidi channel specification %s does not conform to channel<X>.<control><Y>\n", spec);
		return NULL;
	}

	next_token++;

	if(!strncmp(next_token, "cc", 2)){
		ident.fields.type = cc;
		next_token += 2;
	}
	else if(!strncmp(next_token, "note", 4)){
		ident.fields.type = note;
		next_token += 4;
	}
	else if(!strncmp(next_token, "pressure", 8)){
		ident.fields.type = pressure;
		next_token += 8;
	}
	else if(!strncmp(next_token, "pitch", 5)){
		ident.fields.type = pitchbend;
	}
	else if(!strncmp(next_token, "aftertouch", 10)){
		ident.fields.type = aftertouch;
	}
	else{
		fprintf(stderr, "Unknown rtpmidi channel control type in spec %s\n", spec);
		return NULL;
	}

	ident.fields.control = strtoul(next_token, NULL, 10);

	if(ident.label){
		return mm_channel(inst, ident.label, 1);
	}
	return NULL;
}

static int rtpmidi_set(instance* inst, size_t num, channel** c, channel_value* v){
	//TODO
	return 1;
}

static int rtpmidi_handle(size_t num, managed_fd* fds){
	//TODO handle discovery

	if(!num){
		return 0;
	}

	//TODO
	return 1;
}

static int rtpmidi_start(){
	size_t n, u;
	int rv = 1;
	instance** inst = NULL;
	rtpmidi_instance_data* data = NULL;

	//TODO if mdns name defined and no socket, bind default values

	if(cfg.mdns_fd < 0){
		fprintf(stderr, "No mDNS discovery interface bound, AppleMIDI session support disabled\n");
	}

	//fetch all defined instances
	if(mm_backend_instances(BACKEND_NAME, &n, &inst)){
		fprintf(stderr, "Failed to fetch instance list\n");
		return 1;
	}

	for(u = 0; u < n; u++){
		data = (rtpmidi_instance_data*) inst[u]->impl;
		//check whether instances are explicitly configured to a mode
		if(data->mode == unconfigured){
			fprintf(stderr, "rtpmidi instance %s is missing a mode configuration\n", inst[u]->name);
			goto bail;
		}

		//generate random ssrc's
		if(!data->ssrc){
			data->ssrc = rand() << 16 | rand();
		}
	}

	rv = 0;
bail:
	free(inst);
	return rv;
}

static int rtpmidi_shutdown(){
	//TODO cleanup instance data

	free(cfg.mdns_name);
	if(cfg.mdns_fd >= 0){
		close(cfg.mdns_fd);
	}

	return 0;
}
