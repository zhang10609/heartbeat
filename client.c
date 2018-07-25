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
#include <syslog.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <stdbool.h>

#include "list.h"
//#include "logging.h"


//static int ping_initialized = 0;
//struct list_head ping_table;
static pthread_t sendpid, recvpid, checkpid;
//static int fd_count = 0;

#define SERVER_IP   "10.30.0.3"
#define SERVER_PORT 8000
#define CLIENT_IP   "10.30.0.12"
#define CLIENT_PORT 8000

#define HB_LOG_TIMESTR_SIZE 256
#define GF_PRI_SUSECONDS   "06ld"
#define LOG_FILE_PATH      "/var/log/heartbeat.log"

typedef enum {
    HB_LOG_NONE,
    HB_LOG_EMERG,
    HB_LOG_ALERT,
    HB_LOG_CRITICAL,   /* fatal errors */
    HB_LOG_ERROR,      /* major failures (not necessarily fatal) */
    HB_LOG_WARNING,    /* info about normal operation */
    HB_LOG_NOTICE,
    HB_LOG_INFO,       /* Normal information */
    HB_LOG_DEBUG,      /* internal errors */
    HB_LOG_TRACE,      /* full trace of operation */
} hb_loglevel_t;

typedef struct hb_log_handle_{
    pthread_mutex_t logfile_mutex;
    hb_loglevel_t loglevel;
    char *filename;
    FILE *logfile;
} hb_log_handle_t;

typedef struct _heartbeat {
    struct list_head ping_table;
    int ping_initialized;
    int fd_count;
    int heartbeat_exit;
    pthread_mutex_t exit_mtx;
    pthread_mutex_t ping_table_mtx;
    hb_log_handle_t log;
} heartbeat_t;


heartbeat_t *heartbeat = NULL;

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




int _hb_log (const char *domain, const char *file,
             const char *function, int32_t line, hb_loglevel_t level,
             const char *fmt, ...)
             __attribute__ ((__format__ (__printf__, 6, 7)));

#define FMT_WAR(fmt...) do {if (0) printf(fmt);}  while (0)

#define hb_log(dom, level, fmt...) do {                            \
              FMT_WAR (fmt);                                       \
              _hb_log(dom, __FILE__, __FUNCTION__, __LINE__,       \
                      level, ##fmt);                               \
              } while (0)

static bool
skip_logging (hb_loglevel_t level)
{
        bool ret = false;
        hb_loglevel_t existing_level = HB_LOG_NONE;

        if (level == HB_LOG_NONE) {
                ret = true;
                goto out;
        }

        existing_level = heartbeat->log.loglevel;
        if (level > existing_level) {
                ret = true;
                goto out;
        }
out:
        return ret;
}

int
hb_log_init (const char *file)
{
    int fd = -1;
    size_t len = 0;
    int ret = -1;

    len = strlen(file) + 1;
    heartbeat->log.filename = calloc(len, sizeof(char));
    if (!heartbeat->log.filename) {
        fprintf (stderr, "malloc failed, err=%s", strerror(errno));
        ret = -1;
        goto out;
    }

    memcpy (heartbeat->log.filename, file, len);

    fd = open(file, O_CREAT | O_RDONLY, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        fprintf (stderr, "ERROR: failed to create logfile %s\n",
                 strerror(errno));
        ret = -1;
        if (heartbeat->log.filename) {
            free (heartbeat->log.filename);
        }
        goto out;
    }

    if (fd > 0) {
        close(fd);
    }

    heartbeat->log.logfile = fopen(file, "a");
    if (!heartbeat->log.logfile) {
        fprintf (stderr, "ERROR: failed to open logfile %s\n",
                strerror(errno));
        ret = -1;
        goto out;
    }

    heartbeat->log.loglevel = HB_LOG_INFO;
    pthread_mutex_init(&heartbeat->log.logfile_mutex, NULL);

    ret = 0;

out:
    return ret;
}

int
gf_vasprintf (char **string_ptr, const char *format, va_list arg)
{
        va_list arg_save;
        char    *str = NULL;
        int     size = 0;
        int     rv = 0;

        if (!string_ptr || !format)
                return -1;

        va_copy (arg_save, arg);

        size = vsnprintf (NULL, 0, format, arg);
        size++;
        //str = GF_MALLOC (size, gf_common_mt_asprintf);
        str = malloc (size);
        if (str == NULL) {
                /* log is done in GF_MALLOC itself */
                return -1;
        }
        rv = vsnprintf (str, size, format, arg_save);

        *string_ptr = str;
        va_end (arg_save);
        return (rv);
}

int
gf_asprintf (char **string_ptr, const char *format, ...)
{
        va_list arg;
        int     rv = 0;

        va_start (arg, format);
        rv = gf_vasprintf (string_ptr, format, arg);
        va_end (arg);

        return rv;
}

int
_hb_log (const char *domain, const char *file, const char *function, int line,
         hb_loglevel_t level, const char *fmt, ...)
{
        const char    *basename = NULL;
        FILE          *new_logfile = NULL;
        va_list        ap;
        char           timestr[HB_LOG_TIMESTR_SIZE] = {0,};
        struct timeval tv = {0,};
        char          *str1 = NULL;
        char          *str2 = NULL;
        char          *msg  = NULL;
        size_t         len  = 0;
        int            ret  = 0;
        int            fd   = -1;
        struct tm   *tm = NULL;

        //xlator_t      *this = NULL;
        //cetusfs_ctx_t *ctx = NULL;

        //this = THIS;
        //ctx = this->ctx;

        //if (!ctx)
        //        goto out;

        if (skip_logging (level))
                goto out;

        static char *level_strings[] = {"",  /* NONE */
                                        "M", /* EMERGENCY */
                                        "A", /* ALERT */
                                        "C", /* CRITICAL */
                                        "E", /* ERROR */
                                        "W", /* WARNING */
                                        "N", /* NOTICE */
                                        "I", /* INFO */
                                        "D", /* DEBUG */
                                        "T", /* TRACE */
                                        ""};

        if (!domain || !file || !function || !fmt) {
                fprintf (stderr,
                         "logging: %s:%s():%d: invalid argument\n",
                         __FILE__, __PRETTY_FUNCTION__, __LINE__);
                return -1;
        }

        basename = strrchr (file, '/');
        if (basename)
                basename++;
        else
                basename = file;

#if 0
        if (ctx->log.log_control_file_found)
        {
                int priority;
                /* treat GF_LOG_TRACE and GF_LOG_NONE as LOG_DEBUG and
                   other level as is */
                if (GF_LOG_TRACE == level || GF_LOG_NONE == level) {
                        priority = LOG_DEBUG;
                } else {
                        priority = level - 1;
                }

                va_start (ap, fmt);
                vasprintf (&str2, fmt, ap);
                va_end (ap);

                //gf_syslog (priority, "[%s:%d:%s] %d-%s: %s",
                //           basename, line, function,
                //           ((this->graph) ? this->graph->id:0), domain, str2);
                gf_syslog (priority, "[%s:%d:%s] : %s",
                           basename, line, function, str2);
                goto err;
        }

        if (ctx->log.logrotate) {
                ctx->log.logrotate = 0;

                fd = open (ctx->log.filename,
                           O_CREAT | O_RDONLY, S_IRUSR | S_IWUSR);
                if (fd < 0) {
                        gf_msg ("logrotate", GF_LOG_ERROR, errno,
                                LG_MSG_FILE_OP_FAILED,
                                "failed to open logfile");
                        return -1;
                }
                sys_close (fd);

                new_logfile = fopen (ctx->log.filename, "a");
                if (!new_logfile) {
                        gf_msg ("logrotate", GF_LOG_CRITICAL, errno,
                                LG_MSG_FILE_OP_FAILED,
                                "failed to open logfile %s",
                                ctx->log.filename);
                        goto log;
                }

                pthread_mutex_lock (&ctx->log.logfile_mutex);
                {
                        if (ctx->log.logfile)
                                fclose (ctx->log.logfile);

                        ctx->log.gf_log_logfile =
                                ctx->log.logfile = new_logfile;
                }
                pthread_mutex_unlock (&ctx->log.logfile_mutex);

        }
#endif

log:
        ret = gettimeofday (&tv, NULL);
        if (-1 == ret)
                goto out;
        va_start (ap, fmt);
        //gf_time_fmt (timestr, sizeof timestr, tv.tv_sec, gf_timefmt_FT);
        //snprintf (timestr + strlen (timestr), sizeof timestr - strlen (timestr),
         //         ".%"GF_PRI_SUSECONDS, tv.tv_usec);
         tm = localtime (&tv.tv_sec);
         strftime (timestr, 256, "%Y-%m-%d %H:%M:%S", tm);
         snprintf (timestr + strlen (timestr), 256 - strlen (timestr), ".%"GF_PRI_SUSECONDS, tv.tv_usec);

//#ifdef GF_LINUX_HOST_OS
            #define gettid() syscall(__NR_gettid)
//#else
//            #define gettid() 0
//#endif

        //ret = gf_asprintf (&str1, "[%s] %s [%s:%d:%s] tid:%d %d-%s: ",
        //                   timestr, level_strings[level],
        //                   basename, line, function,gettid(),
        //                   ((this->graph)?this->graph->id:0), domain);
        ret = gf_asprintf (&str1, "[%s] %s [%s:%d:%s] tid:%d %s: ",
                           timestr, level_strings[level],
                           basename, line, function, gettid(), domain);
        if (-1 == ret) {
                goto err;
        }

        ret = gf_vasprintf (&str2, fmt, ap);
        if (-1 == ret) {
                goto err;
        }

        va_end (ap);

        len = strlen (str1);
        //msg = GF_MALLOC (len + strlen (str2) + 1, gf_common_mt_char);
        msg = malloc (len + strlen (str2) + 1);
        if (!msg) {
                goto err;
        }

        strcpy (msg, str1);
        strcpy (msg + len, str2);

        //pthread_mutex_lock (&ctx->log.logfile_mutex);
        pthread_mutex_lock (&heartbeat->log.logfile_mutex);
        {

                if (heartbeat->log.logfile) {
                        fprintf (heartbeat->log.logfile, "%s\n", msg);
                        fflush (heartbeat->log.logfile);
                } else if (heartbeat->log.loglevel >= level) {
                        fprintf (stderr, "%s\n", msg);
                        fflush (stderr);
                }

#ifdef GF_LINUX_HOST_OS
                /* We want only serious log in 'syslog', not our debug
                   and trace logs */
                if (ctx->log.gf_log_syslog && level &&
                    (level <= ctx->log.sys_log_level))
                        syslog ((level-1), "%s\n", msg);
#endif
        }
        pthread_mutex_unlock (&heartbeat->log.logfile_mutex);
        //pthread_mutex_unlock (&ctx->log.logfile_mutex);

err:
        free (msg);

        free (str1);

        free (str2);

out:
        va_end (ap);
        return (0);
}


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

void *send_udp_message(void *data)
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

    if (heartbeat == NULL) {
        hb_log("heartbeat", HB_LOG_ERROR, "heartbeat=%p", heartbeat);
        return NULL;
    }

    while(1) {
        if (heartbeat->heartbeat_exit == 1) {
            hb_log("heartbeat", HB_LOG_INFO, "send message thread exit");
            break;
        }

        if(!list_empty(&heartbeat->ping_table)){
            struct ping_entry *entry = NULL;
            struct ping_entry *tmp = NULL;
            pthread_mutex_lock(&heartbeat->ping_table_mtx);
            list_for_each_entry_safe (entry, tmp, &heartbeat->ping_table, list)
            {
                send_flag = 0;
                gettimeofday(&now, NULL);
                elapse_time_us = timediff(now, entry->tv_send);

                if(elapse_time_us > entry->interval && elapse_time_us != ~0U) {
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
                        hb_log("heartbeat", HB_LOG_ERROR, "Send failed, from ip=%s:%d,to ip=%s:%d,err=%s",
                              src_addr, ntohs(entry->src.sin_port),
                              dst_addr, ntohs(entry->dst.sin_port),
                              strerror(errno));
                    } else {
                        entry->tv_send = now;
                        hb_log("heartbeat", HB_LOG_INFO, "Send success, from ip=%s:%d,to ip=%s:%d",
                              src_addr, ntohs(entry->src.sin_port),
                              dst_addr, ntohs(entry->dst.sin_port));
                    }
                }
            }
            pthread_mutex_unlock(&heartbeat->ping_table_mtx);
        }
    }
    return NULL;
}

void recv_udp_packet(int sockfd)
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
        hb_log("heartbeat", HB_LOG_INFO, "Recv udp failed!,err=%s", strerror(errno));
    } else {

        memset(&recv_data, 0, sizeof(recv_data));
        memcpy(&recv_data, ipbuf, sizeof(recv_data)+1);

        if(!list_empty(&heartbeat->ping_table)){
            gettimeofday(&tv, NULL);
            struct ping_entry *entry = NULL;
            struct ping_entry *tmp = NULL;
            pthread_mutex_lock(&heartbeat->ping_table_mtx);
            list_for_each_entry_safe(entry, tmp, &heartbeat->ping_table, list)
            {
                if(entry->src.sin_port != recv_data.dst.sin_port) {
                    strcpy(entry_ip, inet_ntoa(entry->src.sin_addr));
                    strcpy(recv_ip, inet_ntoa(recv_data.dst.sin_addr));
                    hb_log("heartbeat", HB_LOG_DEBUG, "entry=%s:%d,recv=%s:%d", entry_ip, ntohs(entry->src.sin_port),
                                  recv_ip, ntohs(recv_data.dst.sin_port));
                    continue;
                }
                if(entry->src.sin_addr.s_addr != recv_data.dst.sin_addr.s_addr) {
                    continue;
                }
                if(entry->dst.sin_port != recv_data.src.sin_port) {
                    continue;
                }
                if(entry->dst.sin_addr.s_addr != recv_data.src.sin_addr.s_addr) {
                    continue;
                }

                entry->tv_recv = tv;
                hb_log("heartbeat", HB_LOG_DEBUG, "tv_recv=%lu",
                   1000 * 1000 * entry->tv_recv. tv_sec + entry->tv_recv.tv_usec);
            }
            pthread_mutex_unlock(&heartbeat->ping_table_mtx);
        }

        strcpy(ip, inet_ntoa(recv_data.src.sin_addr));
        hb_log("heartbeat", HB_LOG_INFO, "Recv success, src=%s:%d", ip, ntohs(recv_data.src.sin_port));
    }
}


void *check_heartbeat_timeout(void *data)
{
    struct timeval start_tv = {};
    struct timeval end_tv = {};
    struct timeval tv = {};
    struct timeval now = {};
    unsigned long diff = 0;
    unsigned int elapse_time_us = 0;
    int first_flag = 0;

    if (heartbeat == NULL) {
        hb_log("heartbeat", HB_LOG_ERROR, "heartbeat=%p", heartbeat);
        return NULL;
    }

    while(1) {
        gettimeofday(&now, NULL);
        if(!list_empty(&heartbeat->ping_table)){
            struct ping_entry *entry = NULL;
            struct ping_entry *tmp = NULL;
            pthread_mutex_lock(&heartbeat->ping_table_mtx);
            list_for_each_entry_safe(entry, tmp, &heartbeat->ping_table, list)
            {
                if (entry->tv_recv.tv_sec == 0 && entry->tv_recv.tv_usec == 0) {
                    if (first_flag == 0){
                        gettimeofday(&tv, NULL);
                        first_flag = 1;
                    }
                    elapse_time_us = timediff(now, tv);
                    if (elapse_time_us < 3000 * 1000) {
                        goto do_nothing;
                    }
                    hb_log ("heartbeat", HB_LOG_WARNING, "Not recv any packet from server in last 3 seconds!");
                    first_flag = 0;
                }

                diff = timediff(now, entry->tv_recv);
                if(diff > entry->timeout && diff != ~0U){
                    hb_log ("heartbeat", HB_LOG_INFO, "timeout!now=%lu,recv=%lu,diff=%lu",
                      1000 * 1000 * now.tv_sec + now.tv_usec,
                      1000 * 1000 * entry->tv_recv.tv_sec + entry->tv_recv.tv_usec,
                      diff); //us
                }
do_nothing:
;
            }
            pthread_mutex_unlock(&heartbeat->ping_table_mtx);
        }
    }
}

void *recv_udp_message(void *data)
{
    int    ret   = 0;
    struct ping_data recv_data = {0};
    struct udp_socket udp_socket[10];
    int    i     = 0;
    struct pollfd pollfd[100] = {0};

    if(heartbeat == NULL) {
        hb_log("heartbeat", HB_LOG_ERROR, "heartbeat=%p", heartbeat);
        return NULL;
    }

    while (1)
    {
        if (heartbeat->heartbeat_exit == 1) {
            break;
        }

        if(!list_empty(&heartbeat->ping_table))
        {
            int i = 0;
            struct ping_entry *entry = NULL;
            struct ping_entry *tmp = NULL;
            pthread_mutex_lock(&heartbeat->ping_table_mtx);
            list_for_each_entry_safe (entry, tmp, &heartbeat->ping_table, list)
            {
                pollfd[i].fd = entry->sockfd;
                pollfd[i].events = POLLIN;
                i++;
            }
            pthread_mutex_unlock(&heartbeat->ping_table_mtx);

            ret = poll(&pollfd[0], heartbeat->fd_count, 100);
            if(ret < 0){
                hb_log("heartbeat", HB_LOG_ERROR, "poll failed, err=%s", strerror(errno));
                continue;
            }

            for(i = 0;i < heartbeat->fd_count; i++)
            {
                if((pollfd[i].revents & POLLIN) == POLLIN)
                {
                    hb_log("heartbeat", HB_LOG_INFO, "pollfd[%d]=%d",
                           i, pollfd[i].fd);
                    recv_udp_packet(pollfd[i].fd);
                }
            }
        }
    }
    return NULL;
}


void
reconfigure_heartbeat_info(int interval, int timeout)
{
    if (!list_empty(&heartbeat->ping_table)) {
        struct ping_entry *entry = NULL;
        struct ping_entry *tmp = NULL;
        pthread_mutex_lock(&heartbeat->ping_table_mtx);
        list_for_each_entry_safe(entry, tmp, &heartbeat->ping_table, list) {
            entry->interval = interval * 1000;
            entry->timeout = timeout * 1000;
        }
        pthread_mutex_unlock(&heartbeat->ping_table_mtx);
    }

    pthread_mutex_lock(&heartbeat->exit_mtx);
    if (interval == 0) {
        heartbeat->heartbeat_exit = 1;
    }
    pthread_mutex_unlock(&heartbeat->exit_mtx);
}

static int set_fd_nonblock(int sockfd)
{
    int flag = 0;
    int  ret = -1;

    flag = fcntl(sockfd, F_GETFL, 0);
    if(flag < 0)
    {
        hb_log ("heartbeat", HB_LOG_ERROR, "fcntl get flag failed, err=%s", strerror(errno));
        return ret;
    } else {
        ret = fcntl(sockfd, F_SETFL, flag | O_NONBLOCK);
        if (ret == -1){
            hb_log ("heartbeat", HB_LOG_ERROR, "fcntl set flag failed, err=%s", strerror(errno));
        }
    }

   return ret;
}

int register_heartbeat_info(struct sockaddr_in *ssa, struct sockaddr_in *dsa,
                           unsigned int interval, unsigned int timeout)//pid用getpid
{
    struct sockaddr_in *src = NULL;
    struct sockaddr_in *dst = NULL;
    struct ping_entry *entry = NULL;
    int sockfd = 0;
    int ret = -1;
    char src_addr[20] = {0};
    char dst_addr[20] = {0};
    struct timeval tv = {0};

    if(ssa == NULL || dsa == NULL) {
      hb_log("heartbeat", HB_LOG_ERROR, "ssa=%p, dsa=%p",
               ssa, dsa );
        goto out;
    }

    src = ssa;
    dst = dsa;

    entry = (struct ping_entry *)malloc(sizeof(struct ping_entry));//calloc?
    if(entry == NULL){
        hb_log("heartbeat", HB_LOG_ERROR, "malloc failed,err=%s",
               strerror(errno) );
        goto out;
    }

    INIT_LIST_HEAD(&entry->list);

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if(sockfd == -1) {
        hb_log("heartbeat", HB_LOG_ERROR, "create socket failed!");
        goto out;
    }

    ret = set_fd_nonblock(sockfd);
    if(ret == -1) {
        hb_log("heartbeat", HB_LOG_ERROR, "set fd nonblock failed,err=%s",
              strerror(errno));
        goto out;
    }

    ret = bind(sockfd, (struct sockaddr *)src, sizeof(struct sockaddr_in));
    if(ret != 0) {
        hb_log("heartbeat", HB_LOG_ERROR, "bind socket failed,ret = %d, err=%s",
              ret, strerror(errno));
        goto out;
    }

    tv.tv_sec = 0;
    tv.tv_usec = 0;
    entry->src = *src;
    entry->dst = *dst;
    entry->interval = interval * 1000; //us
    entry->timeout = timeout * 1000; //us
    gettimeofday(&entry->tv_send, NULL);
    entry->tv_recv = tv;
    entry->pid = getpid();
    entry->sockfd = sockfd;

    pthread_mutex_lock(&heartbeat->ping_table_mtx);
    list_add_tail(&entry->list, &heartbeat->ping_table);
    heartbeat->fd_count++;
    pthread_mutex_unlock(&heartbeat->ping_table_mtx);

    ret = 0;
    return ret;
out:
    if(entry) {
        free(entry);
    }
    if(sockfd) {
        close(sockfd);
    }
    return ret;

}

int unregister_heartbeat_info(struct sockaddr_in *ssa, struct sockaddr_in *dsa)
{
    struct sockaddr_in *sin = ssa;
    struct sockaddr_in *din = dsa;
    int ret = -1;
    int found = 0;

    if (ssa == NULL || dsa == NULL) {
        hb_log ("heartbeat", HB_LOG_ERROR, "ssa or dsa is null");
        goto out;
    }

    if (!list_empty(&heartbeat->ping_table)) {
        struct ping_entry *entry = NULL;
        struct ping_entry *tmp = NULL;
        pthread_mutex_lock(&heartbeat->ping_table_mtx);
        list_for_each_entry_safe(entry, tmp, &heartbeat->ping_table, list) {
            if(entry->src.sin_port != sin->sin_port) {
                continue;
            }
            if(entry->src.sin_addr.s_addr != sin->sin_addr.s_addr) {
                continue;
            }
            if(entry->dst.sin_port != din->sin_port) {
                continue;
            }
            if(entry->dst.sin_addr.s_addr != din->sin_addr.s_addr) {
                continue;
            }

            found = 1;
            break;
        }

        if (found) {
            close(entry->sockfd);
            list_del_init(&entry->list);
            free(&entry->list);
        }
        pthread_mutex_unlock(&heartbeat->ping_table_mtx);
    }

    ret = 0;
out:
    return ret;
}

int heartbeat_init()
{
    int ret = -1;

    if (heartbeat != NULL) {
        if (heartbeat->ping_initialized == 1) {
            fprintf(stderr, "heartbeat has initialized!\n");
            goto out;
        }
    }

    heartbeat = (heartbeat_t *)calloc(1, sizeof(heartbeat_t));
    if(heartbeat == NULL) {
        fprintf(stderr, "calloc heartbeat failed!\n");
        goto out;
    }

    ret = hb_log_init (LOG_FILE_PATH);
    if (ret) {
        fprintf(stderr, "failed to init log file %s", strerror(errno));
    }

    heartbeat->ping_initialized = 1;
    heartbeat->heartbeat_exit = 0;
    pthread_mutex_init(&heartbeat->ping_table_mtx, NULL);
    pthread_mutex_init(&heartbeat->exit_mtx, NULL);
    INIT_LIST_HEAD(&heartbeat->ping_table);

    ret = pthread_create(&sendpid, NULL, &send_udp_message, NULL);
    if(ret !=0 ){
        hb_log("heartbeat", HB_LOG_ERROR, "create send_udp_message thread failed!");
        goto out;
    }
    ret = pthread_create(&recvpid, NULL, &recv_udp_message, NULL);
    if(ret !=0 ){
        hb_log("heartbeat", HB_LOG_ERROR, "create recv_udp_message thread failed!");
        goto out;
    }
    ret = pthread_create(&checkpid, NULL, &check_heartbeat_timeout, NULL);
    if(ret !=0 ){
        hb_log("heartbeat", HB_LOG_ERROR, "create check_heartbeat_timeout thread failed!");
        goto out;
    }


    ret = 0;
out:
    return ret;
}

int main(int argc,char *argv[])
{
    int ret = -1;
    struct sockaddr_in client_addr = {0};
    struct sockaddr_in server_addr = {0};
    struct sockaddr_in client_addr1 = {0};
    struct sockaddr_in server_addr1 = {0};
    struct sockaddr_in client_addr2 = {0};
    struct sockaddr_in server_addr2 = {0};
    int sockfd = 0;

    bzero(&client_addr, sizeof(client_addr));
    client_addr.sin_family = AF_INET;
    client_addr.sin_addr.s_addr = inet_addr(CLIENT_IP);
    client_addr.sin_port = htons(CLIENT_PORT);

    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    server_addr.sin_port = htons(SERVER_PORT);

    bzero(&client_addr1, sizeof(client_addr1));
    client_addr1.sin_family = AF_INET;
    client_addr1.sin_addr.s_addr = inet_addr(CLIENT_IP);
    client_addr1.sin_port = htons(8001);

    bzero(&server_addr1, sizeof(server_addr1));
    server_addr1.sin_family = AF_INET;
    server_addr1.sin_addr.s_addr = inet_addr(SERVER_IP);
    server_addr1.sin_port = htons(8001);

    bzero(&client_addr2, sizeof(client_addr2));
    client_addr2.sin_family = AF_INET;
    client_addr2.sin_addr.s_addr = inet_addr(CLIENT_IP);
    client_addr2.sin_port = htons(8002);

    bzero(&server_addr2, sizeof(server_addr2));
    server_addr2.sin_family = AF_INET;
    server_addr2.sin_addr.s_addr = inet_addr(SERVER_IP);
    server_addr2.sin_port = htons(8002);

    ret = heartbeat_init();
    ret = register_heartbeat_info(&client_addr, &server_addr, 100, 800);
    ret = register_heartbeat_info(&client_addr1, &server_addr1, 100, 800);
    ret = register_heartbeat_info(&client_addr2, &server_addr2, 100, 800);

    //sleep(10);
    //reconfigure_heartbeat_info(0, 3000);
    //ret = unregister_heartbeat_info(&client_addr, &server_addr);
    //hb_log("heartbeat", HB_LOG_INFO, "ret=%d", ret);
    pthread_join(sendpid,(void*)&send_udp_message);
    pthread_join(recvpid,(void*)&recv_udp_message);
    pthread_join(recvpid,(void*)&check_heartbeat_timeout);
}
