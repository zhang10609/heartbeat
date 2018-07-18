#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>

#include "list.h"


static int ping_initialized = 0;
struct list_head ping_table;
static pthread_mutex_t ping_mutex;
static pthread_t sendpid, recvpid, checkpid;
static int fd_count = 0;

//struct heartbeat {
//    struct list_head ping_table;
//    int    fd_count;
//}

#define SERVER_IP   "10.30.0.3"
#define SERVER_PORT 8000
#define CLIENT_IP   "10.30.0.12"
#define CLIENT_PORT 8000


struct ping_entry {
    struct list_head list;
    struct sockaddr_in src;
    struct sockaddr_in dst;
    struct timeval tv_send;
    struct timeval tv_recv;
    unsigned int    timeout;
    unsigned int    interval;
    int             sockfd;
    int             pid;
};

#pragma pack(1)
struct ping_data {
    struct sockaddr_in src;
    struct sockaddr_in dst;
    int              value;

};
#pragma pack()

struct udp_socket {
    struct pollfd   pollfd;
    int             sockfd;
    struct sockaddr_in src;
};

static unsigned int inline timediff(struct timeval tv1, struct timeval tv2)
{
    unsigned long diff;

    if(tv1.tv_sec < tv2.tv_sec
       || (tv1.tv_sec == tv2.tv_sec && tv1.tv_usec < tv2.tv_usec)) {
        return ~0U;
    }

    tv1.tv_sec -= tv2.tv_sec;
    tv1.tv_usec -= tv2.tv_usec;
    if(tv1.tv_usec < 0) {
        tv1.tv_usec += 1000*1000;
        tv1.tv_sec--;
    }

    diff = tv1.tv_usec + tv1.tv_sec * 1000 * 1000;

    return diff;
}

void *send_udp_packet(void *data)
{
    struct ping_data send_data = {0};
    int sockfd = 0;
    int ret = 0;
    char src_addr[20] = {0};
    char dst_addr[20] = {0};
    struct timeval now = {};
    struct timeval tv = {};
    struct timespec timeout = {};
    unsigned long time = 0;
    unsigned nsec = 0;
    int send_flag = 0;
    unsigned int elapse_time_us = 0;

    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_mutex_init(&mutex, NULL);
    pthread_cond_init(&cond, NULL);

    while(1) {

        if(!list_empty(&ping_table)){
            struct ping_entry *entry = NULL;
            list_for_each_entry (entry, &ping_table, list)
            {
                send_flag = 0;
                gettimeofday(&now, NULL);
                elapse_time_us = timediff(now, entry->tv_send);
                //printf("send=%lu\n", 1000000*now.tv_sec + now.tv_usec);

                if(elapse_time_us > entry->interval) {
                    send_flag = 1;
                } else {
                    continue;
                }

                if (send_flag) {
                    memset(&send_data, 0, sizeof(send_data));
                    send_data.src = entry->src;
                    send_data.dst = entry->dst;
                    send_data.value = 137;
                    sockfd = entry->sockfd;

                    strcpy(src_addr, inet_ntoa(entry->src.sin_addr));
                    strcpy(dst_addr, inet_ntoa(entry->dst.sin_addr));
                    char *send_buf = (char *)&send_data;

                    gettimeofday(&tv, NULL);
                    //printf("tv=%lu\n", 1000 * 1000 * tv.tv_sec + tv.tv_usec);

                    ret = sendto(sockfd, send_buf, sizeof(send_data), 0, (struct sockaddr *)&entry->dst, sizeof(entry->dst));
                    if (ret == -1 || ret < sizeof(data)) {
                        printf("Send failed,from ip=%s:%d,to ip=%s:%d,err=%s\n",
                              src_addr, ntohs(entry->src.sin_port), dst_addr, ntohs(entry->dst.sin_port),strerror(errno));
                    } else {
                        entry->tv_send = now;
                        printf("Send send success,from ip=%s:%d,to ip=%s:%d\n",
                              src_addr, ntohs(entry->src.sin_port), dst_addr, ntohs(entry->dst.sin_port));
                    }
                }
            }
        }

    }
    pthread_mutex_destroy(&mutex);
    pthread_cond_destroy(&cond);
}

void recv_udp_message(int sockfd)
{
    struct sockaddr_in addr;
    char ipbuf[512];
    int  ret = -1;
    struct ping_data recv_data = {0};
    char ip[20] = {0};
    struct timeval tv = {};
    char entry_ip[20] = {0};
    char recv_ip[20] = {0};

    socklen_t addrlen = sizeof(addr);
    memset(ipbuf, 0, sizeof(ipbuf));
    bzero(&addr,sizeof(addr));

    ret = recvfrom(sockfd, ipbuf, sizeof(struct ping_data), 0, (struct sockaddr *)&addr, &addrlen);
    if(ret == -1){
        printf("Recv udp failed!\n");
    } else {

        memset(&recv_data, 0, sizeof(recv_data));
        memcpy(&recv_data, ipbuf, sizeof(recv_data)+1);

        //pthread_mutex_lock(&ping_mutex);
        if(!list_empty(&ping_table)){
            gettimeofday(&tv, NULL);
            struct ping_entry *entry = NULL;
            list_for_each_entry(entry, &ping_table, list)
            {
                if(entry->src.sin_port != recv_data.dst.sin_port){
                    continue;
                }
                if(entry->src.sin_addr.s_addr != recv_data.dst.sin_addr.s_addr){
                    continue;
                }
                if(entry->dst.sin_port != recv_data.src.sin_port){
                    continue;
                }
                if(entry->dst.sin_addr.s_addr != recv_data.src.sin_addr.s_addr){
                    continue;
                }

                entry->tv_recv = tv;
            }
        }
        //pthread_mutex_unlock(&ping_mutex);

        strcpy(ip, inet_ntoa(recv_data.src.sin_addr));
        printf("Recv success,src=%s:%d\n", ip, ntohs(recv_data.src.sin_port));
    }
}


void check_heartbeat_timeout()
{
    struct timeval start_tv = {};
    struct timeval end_tv = {};
    struct timeval tv = {};
    unsigned long diff = 0;

    //while(1){
        gettimeofday(&tv, NULL);
        pthread_mutex_lock(&ping_mutex);
        if(!list_empty(&ping_table)){
            struct ping_entry *entry = NULL;
            list_for_each_entry(entry, &ping_table, list)
            {
                diff = timediff(tv, entry->tv_recv);
                printf("diff=%ld\n", diff);//us
                if(diff > entry->timeout){
                    printf("timed out!diff=%ld\n", diff);//us
                }
            }
        }
        pthread_mutex_unlock(&ping_mutex);
    //}
}

void *recv_udp_packet(void *data)
{
    int    ret   = 0;
    struct ping_data recv_data = {0};
    struct udp_socket udp_socket[10];
    int    i     = 0;
    struct pollfd fds = {0};

    while (1)
    {
        if(!list_empty(&ping_table))
        {
            struct ping_entry *entry = NULL;
            list_for_each_entry (entry, &ping_table, list)//安全:w
            {
                fds.fd = entry->sockfd;
                fds.events = POLLIN;
            }
#if 1
                ret = poll(&fds, fd_count, 1000);
                if(ret < 0){
                    printf("poll failed!\n");
                    continue;
                }

                for(i = 0;i < fd_count; i++)
                {
                    if((fds.revents & POLLIN) == POLLIN)
                    {
                        recv_udp_message(fds.fd);
                    }
                }
                check_heartbeat_timeout();
#endif
        }
    }
}

int heartbeat_init()
{
    int ret = -1;

    if (ping_initialized){ //放到结构体里，加锁j
        return 0;
    }

    pthread_mutex_init(&ping_mutex, NULL);
    INIT_LIST_HEAD(&ping_table);
    ret = pthread_create(&sendpid, NULL, &send_udp_packet, NULL);
    if(ret !=0 ){
        printf("create send_udp_packet thread failed!\n");
        goto out;
    }
    ret = pthread_create(&recvpid, NULL, &recv_udp_packet, NULL);
    if(ret !=0 ){
        printf("create recv_udp_packet thread failed!\n");
        goto out;
    }
    //ret = pthread_create(&checkpid, NULL, &check_heartbeat_timeout, NULL);
    //if(ret !=0 ){
    //    printf("create check heartbeat timeout thread failed!\n");
    //    goto out;
    //}

    ping_initialized = 1;

    ret = 0;
out:
    return ret;
}

void reconfigure_interval_and_timeout(int interval, int timeout)
{

}

static int set_fd_nonblock(int sockfd)
{
    int flag = 0;
    int  ret = -1;

    flag = fcntl(sockfd, F_GETFL, 0);
    if(flag < 0)
    {
        printf("fcntl get flag failed!,err=%s\n", strerror(errno));
        return ret;
    } else {
        ret = fcntl(sockfd, F_SETFL, flag | O_NONBLOCK);
        if (ret == -1){
            printf("fcntl set flag failed,err=%s\n", strerror(errno));
        }
    }

   return ret;
}

int register_raw_ping_info(struct sockaddr_in *ssa, struct sockaddr_in *dsa,
                           unsigned int interval, unsigned int timeout, int pid)//pid用getpid
{
    struct sockaddr_in *src = NULL;
    struct sockaddr_in *dst = NULL;
    struct ping_entry *entry = NULL;
    int sockfd = 0;
    int ret = -1;
    char src_addr[20] = {0};
    char dst_addr[20] = {0};
    struct timeval tv = {0};

    src = ssa;//指针为空判断
    dst = dsa;

    entry = (struct ping_entry *)malloc(sizeof(struct ping_entry));//calloc?
    if(entry == NULL){
        printf("malloc entry failed!\n");
        goto out;
    }
    INIT_LIST_HEAD(&entry->list);

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if(sockfd == -1) {
        printf("create socket failed!\n");
        goto out;
    }

    ret = set_fd_nonblock(sockfd);
    if(ret == -1){
        printf("set fd nonblock failed,err=%s\n", strerror(errno));
        close(sockfd);
        goto out;
    }

    ret = bind(sockfd, (struct sockaddr *)src, sizeof(struct sockaddr_in));
    if(ret != 0){
        printf("bind socket failed,ret=%d,error=%s\n",ret, strerror(errno));
        close(sockfd);
        goto out;
    }

    tv.tv_sec = 0;
    tv.tv_usec = 0;
    entry->src = *src;
    entry->dst = *dst;
    entry->interval = interval * 1000; //us
    entry->timeout = timeout * 1000; //us
    gettimeofday(&entry->tv_send, NULL);
    //entry->tv_recv = tv;
    entry->pid = pid;
    entry->sockfd = sockfd;

    pthread_mutex_lock(&ping_mutex);
    list_add_tail(&entry->list, &ping_table);
    fd_count++;//struct
    pthread_mutex_unlock(&ping_mutex);

    ret = 0;
    return ret;
out:
    if(entry) {
        free(entry);
    }
    return ret;

}


int unregister_raw_ping_info()
{

}


int old_poll()
{
	int udpfd = 0;
	int ret = 0;
	struct pollfd fds[2];
	struct sockaddr_in saddr;
	struct sockaddr_in caddr;

	bzero(&saddr,sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_port   = htons(8000);
	//saddr.sin_addr.s_addr = htonl(INADDR_ANY);

	bzero(&caddr,sizeof(caddr));
	caddr.sin_family  = AF_INET;
	caddr.sin_port    = htons(8000);
    caddr.sin_addr.s_addr = inet_addr("10.30.0.3");

	if((udpfd = socket(AF_INET,SOCK_DGRAM, 0)) < 0)
	{
		perror("socket error");
		exit(-1);
	}

	if(bind(udpfd, (struct sockaddr*)&saddr, sizeof(saddr)) != 0)
	{
		perror("bind error");
		close(udpfd);
		exit(-1);
	}

	printf("input: \"sayto 192.168.220.X\" to sendmsg to somebody\033[32m\n");
	fds[0].fd = 0;
	fds[1].fd = udpfd;

	fds[0].events = POLLIN;
    fds[1].events = POLLIN;

	while(1)
	{
        ret = poll(fds, 2, -1);

		//write(1,"UdpQQ:",6);

        if(ret == -1){
            perror("poll()");
        }
		else if(ret > 0){
            char buf[100] = "zj udp test";
            if((fds[0].revents & POLLIN) == POLLIN ){
				sendto(udpfd, buf, strlen(buf),0,(struct sockaddr*)&caddr, sizeof(caddr));
                sleep(5);
                printf("zj,send to ip=%s:%d\n",inet_ntoa(caddr.sin_addr), ntohs(caddr.sin_port));

            }
			else if((fds[1].revents & POLLIN) == POLLIN ){
                struct sockaddr_in addr;
				char ipbuf[INET_ADDRSTRLEN] = "";
				socklen_t addrlen = sizeof(addr);

				bzero(&addr,sizeof(addr));

				recvfrom(udpfd, buf, 100, 0, (struct sockaddr*)&addr, &addrlen);
				printf("zj recv from ip=%s:%d,%s\n",inet_ntoa(addr.sin_addr), ntohs(addr.sin_port),buf);
            }

       }
	   else if(0 == ret){
            printf("time out\n");
       }
    }

}

int main(int argc,char *argv[])
{
    int ret = -1;
    struct sockaddr_in client_addr = {0};
    struct sockaddr_in server_addr = {0};
    int sockfd = 0;

    bzero(&client_addr, sizeof(client_addr));
    client_addr.sin_family = AF_INET;
    client_addr.sin_addr.s_addr = inet_addr(CLIENT_IP);
    client_addr.sin_port = htons(CLIENT_PORT);

    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    server_addr.sin_port = htons(SERVER_PORT);

    ret = heartbeat_init();
    ret = register_raw_ping_info(&client_addr, &server_addr, 100, 800, 0);

    pthread_join(sendpid,(void*)&send_udp_packet);
    pthread_join(recvpid,(void*)&recv_udp_packet);
    //pthread_join(recvpid,(void*)&check_heartbeat_timeout);

}
