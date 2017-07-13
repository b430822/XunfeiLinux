#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../include/msp_cmn.h"
#include "../include/msp_errors.h"
#include "../include/qivw.h"

void start(void (*recall)(void));
void init_awaken(); //初始化唤醒参数
void init_record(); //初始化录音参数

// int cb_ivw_msg_proc(char *, int, int, int, void *, void *); //唤醒回调

int rc;                        //录音缓存实际大小
int size;                      //录音缓存容量
char *buffer = NULL;           //录音缓存
snd_pcm_t *handle = NULL;      //录音句柄
int is_awaken = 0;             //唤醒跳出条件
const char *session_id = NULL; //唤醒id
snd_pcm_uframes_t frames = NULL;
unsigned int val = 0;

int err_code = MSP_SUCCESS;
char sse_hints[128];

/*
唤醒回调
*/
int cb_ivw_msg_proc(const char *sessionID, int msg, int param1, int param2,
                    const void *info, void *userData) {
  if (MSP_IVW_MSG_ERROR == msg) //唤醒出错消息
  {
    printf("\n\nMSP_IVW_MSG_ERROR errCode = %d\n\n", param1);
  } else if (MSP_IVW_MSG_WAKEUP == msg) //唤醒成功消息
  {
    printf("\n\nMSP_IVW_MSG_WAKEUP result = %s\n\n", info);
  }else{
      printf("\n\nMSP_IVW_MSG_WAKEUP result\n\n");
  }
  return 0;
}

void init_record() {
  fprintf(stderr, "init_record\n");
  snd_pcm_hw_params_t *params;
  int dir = 0;

  rc = snd_pcm_open(&handle, "default", SND_PCM_STREAM_CAPTURE, 0);
  if (rc < 0) {
    fprintf(stderr, "unable to open pcm device :%s\n", snd_strerror(rc));
    exit(1);
  }
  /* */
  /* Allocate a hardware parameters object. */
  snd_pcm_hw_params_alloca(&params);
  /* Fill it in with default values. */
  snd_pcm_hw_params_any(handle, params);
  /* Set the desired hardware parameters. */
  /* Interleaved mode */
  snd_pcm_hw_params_set_access(handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);

  /* Signed 16-bit little-endian format */
  snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_S16_LE);
  /* One channels (stereo) */
  snd_pcm_hw_params_set_channels(handle, params, 1);

  /* 16000 bits/second sampling rate (CD quality) */
  val = 16000;
  snd_pcm_hw_params_set_rate_near(handle, params, &val, &dir);

  /* Set period size to 320 frames. */
  frames = 320;
  snd_pcm_hw_params_set_period_size_near(handle, params, &frames, &dir);

  /* Write the parameters to the driver */
  rc = snd_pcm_hw_params(handle, params);
  if (rc < 0) {
    fprintf(stderr, "unable to set hw parameters: %s\n", snd_strerror(rc));
    exit(1);
  }

  /* Use a buffer large enough to hold one period */
  snd_pcm_hw_params_get_period_size(params, &frames, &dir);
  size = frames * 2; /* 2 bytes/sample, 1 channels */

  // /* We want to loop for 5 seconds */
  snd_pcm_hw_params_get_period_time(params, &val, &dir);
}

/*


*/
void init_awaken() {

  fprintf(stderr, "init_awakne \n");
  int ret = MSP_SUCCESS;
  char *grammar_list = NULL;
  char *session_begin_params = NULL;

  const char *lgi_param =
      "appid = 595c9d5c,engine_start = ivw,ivw_res_path "
      "=fo|res/ivw/wakeupresource.jet, work_dir = ."; //使用唤醒需要在此设置engine_start
                                                      //= ivw,ivw_res_path
                                                      //=fo|xxx/xx 启动唤醒引擎
  const char *ssb_param = "ivw_threshold=0:-20,sst=wakeup";

  ret = MSPLogin(NULL, NULL, lgi_param);
  if (MSP_SUCCESS != ret) {
    fprintf(stderr, "MSPLogin failed, error code: %d.\n", ret);
    exit(1); //登录失败，退出登录
  }
  session_id = QIVWSessionBegin(grammar_list, session_begin_params, &err_code);
  if (err_code != MSP_SUCCESS) {
    fprintf(stderr, "QIVWSessionBegin failed! error code:%d\n", err_code);
    exit(1);
  }

  err_code = QIVWRegisterNotify(session_id, cb_ivw_msg_proc, NULL);
  if (err_code != MSP_SUCCESS) {
    snprintf(sse_hints, sizeof(sse_hints), "QIVWRegisterNotify errorCode=%d",
             err_code);
    fprintf(stderr, "QIVWRegisterNotify failed! error code:%d\n", err_code);
    exit(1);
  }
}

/*


*/
void start_awaken() {
  /*start record*/
  long loops = 5000000 / val;
  fprintf(stderr, "start_awaken\n");
  buffer = (char *)malloc(size);
  while (loops > 0) {
    loops--;
    rc = snd_pcm_readi(handle, buffer, frames);
    if (rc == -EPIPE) {
      /* EPIPE means overrun */
      fprintf(stderr, "overrun occurred\n");
      snd_pcm_prepare(handle);
    } else if (rc < 0) {
      fprintf(stderr, "error from read: %s\n", snd_strerror(rc));
    } else if (rc != (int)frames) {
      fprintf(stderr, "short read, read %d frames\n", rc);
    }
    if (rc != size) {
      fprintf(stderr, "short write: wrote %d bytes,size %d\n", rc,size);
    }

    // rc = write(1, buffer, size);
    err_code = QIVWAudioWrite(session_id, (const void *)&buffer, rc,
                              MSP_AUDIO_SAMPLE_LAST);
    if (MSP_SUCCESS != err_code) {
      fprintf(stderr, "QIVWAudioWrite failed! error code:%d\n", err_code);
      snprintf(sse_hints, sizeof(sse_hints), "QIVWAudioWrite errorCode=%d",
               err_code);
      exit(1);
    }
  }
    fprintf(stderr, "usleep 3s \n");
  usleep(12*1000*1000);
}

int main(int argc, char const *argv[]) {
  init_record();
  init_awaken();
  start_awaken();
  return 0;
}
