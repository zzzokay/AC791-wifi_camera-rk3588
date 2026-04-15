#include "app_config.h"

#if CONFIG_MP3_DEC_ENABLE

#include "device/device.h"
#include "system/includes.h"
#include "server/audio_server.h"
#include "storage_device.h"
#include "wifi/sd_audio_play.h"

#define SD_AUDIO_TASK_NAME   "play_sd_audio_task"
#define SD_AUDIO_STOP_CMD    1001

static OS_MUTEX sd_audio_mutex;
static OS_SEM sd_audio_sem;

struct sd_audio_play_hdl {
    struct server *dec_server;
    char file_path[128];
    char dec_type[8];
    u8 dec_volume;
    int task_pid;
    FILE *file;
    u8 dec_open_flag;
};

static struct sd_audio_play_hdl *sd_audio = NULL;

static void sd_audio_event_handler(void *priv, int argc, int *argv)
{
    int msg = 0;

    switch (argv[0]) {
    case AUDIO_SERVER_EVENT_ERR:
        printf("sd_audio: AUDIO_SERVER_EVENT_ERR\n");
        msg = SD_AUDIO_STOP_CMD;
        os_taskq_post(os_current_task(), 1, msg);
        break;

    case AUDIO_SERVER_EVENT_END:
        printf("sd_audio: AUDIO_SERVER_EVENT_END\n");
        msg = SD_AUDIO_STOP_CMD;
        os_taskq_post(os_current_task(), 1, msg);
        break;

    case AUDIO_SERVER_EVENT_CURR_TIME:
        // printf("sd_audio play_time: %d\n", argv[1]);
        break;

    default:
        break;
    }
}

static void play_sd_audio_task(void *priv)
{
    int msg[16] = {0};
    int err = 0;
    union audio_req req = {0};

    printf("sd_audio: start, path=%s, type=%s, vol=%d\n",
           sd_audio->file_path, sd_audio->dec_type, sd_audio->dec_volume);

    if (!storage_device_ready()) {
        printf("sd_audio: storage not ready\n");
        err = -1;
        goto __fail_notify;
    }

    sd_audio->file = fopen(sd_audio->file_path, "r");
    if (!sd_audio->file) {
        printf("sd_audio: fopen fail -> %s\n", sd_audio->file_path);
        err = -2;
        goto __fail_notify;
    }

    sd_audio->dec_server = server_open("audio_server", "dec");
    if (!sd_audio->dec_server) {
        printf("sd_audio: open audio_server fail\n");
        err = -3;
        goto __fail;
    }

    server_register_event_handler(sd_audio->dec_server, NULL, sd_audio_event_handler);

    req.dec.cmd             = AUDIO_DEC_OPEN;
    req.dec.volume          = sd_audio->dec_volume;
    req.dec.output_buf      = NULL;
    req.dec.output_buf_len  = 12 * 1024;
    req.dec.channel         = 0;
    req.dec.sample_rate     = 0;
    req.dec.priority        = 1;
    req.dec.vfs_ops         = NULL;
    req.dec.file            = sd_audio->file;
    req.dec.dec_type        = sd_audio->dec_type;
    req.dec.sample_source   = "dac";

    if (server_request(sd_audio->dec_server, AUDIO_REQ_DEC, &req) != 0) {
        printf("sd_audio: AUDIO_DEC_OPEN fail\n");
        err = -4;
        goto __fail;
    }

    req.dec.cmd = AUDIO_DEC_START;
    if (server_request(sd_audio->dec_server, AUDIO_REQ_DEC, &req) != 0) {
        printf("sd_audio: AUDIO_DEC_START fail\n");
        err = -5;
        goto __fail;
    }

    sd_audio->dec_open_flag = 1;
    os_sem_post(&sd_audio_sem);

    os_task_pend("taskq", msg, ARRAY_SIZE(msg));

    req.dec.cmd = AUDIO_DEC_STOP;
    printf("sd_audio: stop dec\n");
    server_request(sd_audio->dec_server, AUDIO_REQ_DEC, &req);

    if (sd_audio->file) {
        fclose(sd_audio->file);
        sd_audio->file = NULL;
    }

    if (sd_audio->dec_server) {
        server_close(sd_audio->dec_server);
        sd_audio->dec_server = NULL;
    }

    sd_audio->dec_open_flag = 0;
    os_sem_post(&sd_audio_sem);
    return;

__fail:
    if (sd_audio->file) {
        fclose(sd_audio->file);
        sd_audio->file = NULL;
    }

    if (sd_audio->dec_server) {
        server_close(sd_audio->dec_server);
        sd_audio->dec_server = NULL;
    }

__fail_notify:
    sd_audio->dec_open_flag = 0;
    os_sem_post(&sd_audio_sem);
    return;
}

int sd_audio_play_busy(void)
{
    if (!sd_audio) {
        return 0;
    }
    return sd_audio->dec_open_flag;
}

int sd_audio_play_stop(void)
{
    int msg = SD_AUDIO_STOP_CMD;

    if (!sd_audio) {
        return -1;
    }

    os_mutex_pend(&sd_audio_mutex, 0);

    if (!sd_audio->dec_open_flag) {
        os_mutex_post(&sd_audio_mutex);
        return 0;
    }

    os_taskq_post(SD_AUDIO_TASK_NAME, 1, msg);
    os_sem_pend(&sd_audio_sem, 0);
    thread_kill(&sd_audio->task_pid, KILL_WAIT);

    os_mutex_post(&sd_audio_mutex);
    return 0;
}

int sd_audio_play_start(const char *path, const char *dec_type, u8 volume)
{
    int msg = SD_AUDIO_STOP_CMD;
    int ret = 0;

    if (!sd_audio || !path || !dec_type) {
        return -1;
    }

    os_mutex_pend(&sd_audio_mutex, 0);

    if (!storage_device_ready()) {
        printf("sd_audio: storage not ready in start()\n");
        ret = -2;
        goto __out;
    }

    memset(sd_audio->file_path, 0, sizeof(sd_audio->file_path));
    memset(sd_audio->dec_type, 0, sizeof(sd_audio->dec_type));

    snprintf(sd_audio->file_path, sizeof(sd_audio->file_path), "%s", path);
    snprintf(sd_audio->dec_type, sizeof(sd_audio->dec_type), "%s", dec_type);
    sd_audio->dec_volume = volume;

    if (sd_audio->dec_open_flag == 0) {
        thread_fork(SD_AUDIO_TASK_NAME, 10, 1024, 32,
                    &sd_audio->task_pid, play_sd_audio_task, NULL);
        os_sem_pend(&sd_audio_sem, 0);
    } else {
        os_taskq_post(SD_AUDIO_TASK_NAME, 1, msg);
        os_sem_pend(&sd_audio_sem, 0);
        thread_kill(&sd_audio->task_pid, KILL_WAIT);

        thread_fork(SD_AUDIO_TASK_NAME, 10, 1024, 32,
                    &sd_audio->task_pid, play_sd_audio_task, NULL);
        os_sem_pend(&sd_audio_sem, 0);
    }

    ret = sd_audio->dec_open_flag ? 0 : -3;

__out:
    os_mutex_post(&sd_audio_mutex);
    return ret;
}

static void sd_audio_play_init(void)
{
    os_sem_create(&sd_audio_sem, 0);
    os_mutex_create(&sd_audio_mutex);
    sd_audio = (struct sd_audio_play_hdl *)calloc(1, sizeof(struct sd_audio_play_hdl));
}
late_initcall(sd_audio_play_init);

#endif
