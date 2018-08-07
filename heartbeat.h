#ifndef _HEARTBEAT_H
#define _HEARTBEAT_H

#include <sys/socket.h>
#include <pthread.h>
#include <netinet/in.h>

typedef void (*heartbeat_callback) (struct sockaddr_in *src, struct sockaddr_in *dst,void *msg);

int heartbeat_init();
int register_heartbeat_info (struct sockaddr_in *ssa, struct sockaddr_in *dsa, unsigned int interval,
                             unsigned int timeout, heartbeat_callback callback, void *msg);
int unregister_heartbeat_info (struct sockaddr_in *ssa, struct sockaddr_in *dsa);
int reconfigure_heartbeat_info (unsigned int interval, unsigned int timeout);

typedef struct {
	const char *module;
	int (*init)();
	int (*register_info)(struct sockaddr_in *, struct sockaddr_in *dsa, unsigned int, unsigned int, heartbeat_callback,void *);
	int (*unregister_info)(struct sockaddr_in *, struct sockaddr_in *);
	int (*reconfigure_info)(unsigned int, unsigned int);
}heartbeat_API;

const heartbeat_API HeartBeat = {
	.module = "HeartBeat",
	.init = heartbeat_init,
	.register_info = register_heartbeat_info,
	.unregister_info = unregister_heartbeat_info,
	.reconfigure_info = reconfigure_heartbeat_info
};

#endif
