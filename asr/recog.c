#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/msp_cmn.h"
#include "../include/msp_errors.h"
#include "../include/qisr.h"
#include "../include/qivw.h"

#define SAMPLE_RATE_16K (16000)
#define MAX_GRAMMARID_LEN (32)
#define MAX_PARAMS_LEN (1024)

const char *ASR_RES_PATH = "fo|res/asr/common.jet"; //离线语法识别资源路径
const char *GRM_BUILD_PATH =
    "res/asr/GrmBuilld"; //构建离线语法识别网络生成数据保存路径

typedef struct _UserData {
  int build_fini;  //标识语法构建是否完成
  int update_fini; //标识更新词典是否完成
  int errcode;     //记录语法构建或更新词典回调错误码
  char grammar_id[MAX_GRAMMARID_LEN]; //保存语法构建返回的语法ID
} UserData;
UserData asr_data;

int build_grammar(UserData *udata);  //构建离线识别语法网络
int update_lexicon(UserData *udata); //更新离线识别语法词典
int run_asr(UserData *udata);        //进行离线语法识别
int init_asr(void);                  //进行初始化操作
int start_asr(void);                 //开始识别
int start_recog_thread(void);
int start_recog(void);

char *asr_result_key = "<operate id=\""; //识别结果关键字匹配

void init_awaken();         //初始化唤醒参数
void init_record();         //初始化录音参数
void get_record(char *);    //获取录音
void do_asr_result(char *); //操作识别结果

int size;                      //录音缓存容量
char *buffer;                  //录音缓存
snd_pcm_t *handle;             //录音句柄
volatile int is_awaken = 0;    //唤醒跳出条件
const char *session_id = NULL; //唤醒id
snd_pcm_uframes_t frames;

char *GRAMMAR_FILE = "command.bnf";
int err_code = MSP_SUCCESS;
char sse_hints[128];

pthread_mutex_t mutex;
int timer_count;
pthread_t timer_id;               //计时器线程id
int is_stop_listening = 0;        //循环识别监听条件
void *timer_task(void);           //计时器方法
const int TIMER_COUNT_START = 10; //计时器10s
int asr_break = 0;                //识别循环跳出标志
/*
唤醒回调
*/
int cb_ivw_msg_proc(const char *sessionID, int msg, int param1, int param2,
                    const void *info, void *userData) {
  switch (msg) {
  case MSP_IVW_MSG_ISR_RESULT: //唤醒识别出错消息
    fprintf(stderr, "\nMSP_IVW_MSG_ISR_RESULT result = %s\n", info);
    is_awaken = 1;
    start_timer();
    break;
  case MSP_IVW_MSG_ERROR: //唤醒出错消息
    fprintf(stderr, "\nMSP_IVW_MSG_ERROR errCode = %d\n", param1);
    break;
  case MSP_IVW_MSG_WAKEUP: //唤醒成功消息
    fprintf(stderr, "\nMSP_IVW_MSG_WAKEUP result = %s\n", info);
    is_awaken = 1;
    start_timer();
    break;
  default:
    fprintf(stderr, "\nGet recall from awaken\n");
    break;
    return 0;
  }
}
/*
开启计时器线程
*/
void start_timer() {
  fprintf(stderr, "start_timer...\n");
  is_stop_listening = 0;
  asr_break = 0;
  timer_count = TIMER_COUNT_START;
  pthread_create(&timer_id, NULL, timer_task, NULL);
}

/*
初始化话筒监听参数
*/
void init_record() {
  fprintf(stderr, "init_record...\n");
  snd_pcm_hw_params_t *params;
  int dir = 0;

  int rc = snd_pcm_open(&handle, "default", SND_PCM_STREAM_CAPTURE, 0);
  if (rc < 0) {
    fprintf(stderr, "unable to open pcm device :%s\n", snd_strerror(rc));
    exit(1);
  }
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

  /* 16000 bits/second sampling rate  */
  unsigned int val = 16000;
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
}

/*

初始化唤醒参数
*/
void init_awaken() {
  err_code = MSP_SUCCESS;
  is_awaken = 0;
  fprintf(stderr, "init_awakne \n");
  int ret = MSP_SUCCESS;
  // char *grammar_list = NULL;
  const char *lgi_param =
      "appid = 595c9d5c,engine_start = ivw,ivw_res_path "
      "=fo|res/ivw/wakeupresource.jet, work_dir = ."; //使用唤醒需要在此设置engine_start
                                                      //= ivw,ivw_res_path
                                                      //=fo|xxx/xx 启动唤醒引擎
  const char *session_begin_params = "ivw_threshold=0:-20,sst=wakeup";

  ret = MSPLogin(NULL, NULL, lgi_param);
  if (MSP_SUCCESS != ret) {
    fprintf(stderr, "MSPLogin failed, error code: %d.\n", ret);
    exit(1); //登录失败，退出登录
  }
  session_id = QIVWSessionBegin(GRAMMAR_FILE, session_begin_params, &err_code);
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
获取录音
*/
void get_record(char *buffer) {
  if (buffer == NULL) {
    buffer = (char *)malloc(size);
  }
  int rc = snd_pcm_readi(handle, buffer, frames);
  if (rc == -EPIPE) {
    /* EPIPE means overrun */
    fprintf(stderr, "overrun occurred\n");
    snd_pcm_prepare(handle);
  } else if (rc < 0) {
    fprintf(stderr, "error from read: %s\n", snd_strerror(rc));
  } else if (rc != (int)frames) {
    fprintf(stderr, "short read, read %d frames\n", rc);
  }
}
/*


*/
void start_awaken() {
  /*start record*/

  fprintf(stderr, "start_awaken\n");
  buffer = (char *)malloc(size);
  int audio_stat = MSP_AUDIO_SAMPLE_FIRST;
  fprintf(stderr, "listening... \n");
  while (!is_awaken) {
    get_record(buffer);
    err_code = QIVWAudioWrite(session_id, buffer, size, audio_stat);
    if (MSP_SUCCESS != err_code) {
      fprintf(stderr, "QIVWAudioWrite failed! error code:%d\n", err_code);
      snprintf(sse_hints, sizeof(sse_hints), "QIVWAudioWrite errorCode=%d",
               err_code);
      exit(1);
    }
    audio_stat = MSP_AUDIO_SAMPLE_CONTINUE;
  }
  fprintf(stderr, "break listening ,awaken \n");
  MSPLogout();
}

int build_grm_cb(int ecode, const char *info, void *udata) {
  UserData *grm_data = (UserData *)udata;

  if (NULL != grm_data) {
    grm_data->build_fini = 1;
    grm_data->errcode = ecode;
  }

  if (MSP_SUCCESS == ecode && NULL != info) {
    if (NULL != grm_data)
      snprintf(grm_data->grammar_id, MAX_GRAMMARID_LEN - 1, info);
  } else
    printf("build grammar failed!%d\n", ecode);

  return 0;
}

int build_grammar(UserData *udata) {
  FILE *grm_file = NULL;
  char *grm_content = NULL;
  unsigned int grm_cnt_len = 0;
  char grm_build_params[MAX_PARAMS_LEN] = {NULL};
  int ret = 0;

  grm_file = fopen(GRAMMAR_FILE, "rb");
  if (NULL == grm_file) {
    printf("open\"%s\"file failed![%s]\n", GRAMMAR_FILE, strerror(errno));
    return -1;
  }

  fseek(grm_file, 0, SEEK_END);
  grm_cnt_len = ftell(grm_file);
  fseek(grm_file, 0, SEEK_SET);

  grm_content = (char *)malloc(grm_cnt_len + 1);
  if (NULL == grm_content) {
    printf("malloc failed!\n");
    fclose(grm_file);
    grm_file = NULL;
    return -1;
  }
  fread((void *)grm_content, 1, grm_cnt_len, grm_file);
  grm_content[grm_cnt_len] = '\0';
  fclose(grm_file);
  grm_file = NULL;

  snprintf(grm_build_params, MAX_PARAMS_LEN - 1, "engine_type = local, \
		asr_res_path = %s, sample_rate = %d, \
		grm_build_path = %s, ",
           ASR_RES_PATH, SAMPLE_RATE_16K, GRM_BUILD_PATH);
  ret = QISRBuildGrammar("bnf", grm_content, grm_cnt_len, grm_build_params,
                         build_grm_cb, udata);

  free(grm_content);
  grm_content = NULL;

  return ret;
}

/*
初始化命令识别
*/
int init_asr(void) {
  const char *login_config = "appid = 595c9d5c"; //登录参数
  int ret = 0;
  char c;
  fprintf(stderr, "init_asr...\n");
  ret = MSPLogin(
      NULL, NULL,
      login_config); //第一个参数为用户名，第二个参数为密码，传NULL即可，第三个参数是登录参数
  if (MSP_SUCCESS != ret) {
    fprintf(stderr, "login faile：%d\n", ret);
    exit(1);
  }

  memset(&asr_data, 0, sizeof(UserData));
  ret = build_grammar(
      &asr_data); //第一次使用某语法进行识别，需要先构建语法网络，获取语法ID，之后使用此语法进行识别，无需再次构建
  if (MSP_SUCCESS != ret) {
    fprintf(stderr, "build grammar failed！\n");
    exit(1);
  }
  while (1 != asr_data.build_fini)
    usleep(300 * 1000);
  if (MSP_SUCCESS != asr_data.errcode)
    exit(1);
  return ret;
}

/*
开始识别
*/
int start_asr(void) {
  fprintf(stderr, "start_asr...\n");
  int ret = 0;
  do {
    // fprintf(stderr, "\nrun_asr is again\n");
    ret = run_asr(&asr_data);
  } while (!is_stop_listening);
  asr_break = 1;
  MSPLogout();
  return ret;
}

int run_asr(UserData *udata) {
  char asr_params[MAX_PARAMS_LEN] = {NULL};
  int ep_status = MSP_EP_LOOKING_FOR_SPEECH;
  int rec_status = MSP_REC_STATUS_INCOMPLETE;
  int errcode = -1;
  const char *rec_rslt = NULL;
  int rss_status = MSP_REC_STATUS_INCOMPLETE;

  //离线语法识别参数设置
  snprintf(asr_params, MAX_PARAMS_LEN - 1, "engine_type = local, \
		asr_res_path = %s, sample_rate = %d, \
		grm_build_path = %s, local_grammar = %s, \
		result_type = xml, result_encoding = UTF-8, vad_eos = 100 ,asr_threshold = 30 ,asr_denoise = 1",
           ASR_RES_PATH, SAMPLE_RATE_16K, GRM_BUILD_PATH, udata->grammar_id);
  session_id = QISRSessionBegin(NULL, asr_params, &errcode);
  if (NULL == session_id) {
    exit(1);
  }
  int aud_stat = MSP_AUDIO_SAMPLE_FIRST;
  int count = 0;
  while (1) {
    get_record(buffer);
    fprintf(stderr, ">");
    errcode = QISRAudioWrite(session_id, buffer, size, aud_stat, &ep_status,
                             &rec_status);
    if (MSP_SUCCESS != errcode) {
      exit(1);
    }
    aud_stat = MSP_AUDIO_SAMPLE_CONTINUE;
    if (ep_status == MSP_EP_AFTER_SPEECH) {
      break;
    }
    if (MSP_EP_TIMEOUT == ep_status || MSP_EP_ERROR == ep_status ||
        MSP_EP_MAX_SPEECH == ep_status) {
      fprintf(stderr, "ep_status error %d\n", ep_status);
    }
  }
  // //主动点击音频结束
  QISRAudioWrite(session_id, (const void *)NULL, 0, MSP_AUDIO_SAMPLE_LAST,
                 &ep_status, &rec_status);

  //获取识别结果
  while (MSP_REC_STATUS_COMPLETE != rss_status && MSP_SUCCESS == errcode) {
    rec_rslt = QISRGetResult(session_id, &rss_status, 0, &errcode);
    usleep(10 * 1000);
  }
  if (NULL != rec_rslt) {
    do_asr_result(rec_rslt);
  } else{

  }
  QISRSessionEnd(session_id, NULL);
  return errcode;
}

void do_asr_result(char *rec_rslt) {
  pthread_mutex_lock(&mutex); //计时器重置
  is_stop_listening = 0;
  timer_count = TIMER_COUNT_START;
  pthread_mutex_unlock(&mutex);
  //获取结果id
  rec_rslt = strstr(rec_rslt, asr_result_key);
  if (rec_rslt != NULL) {
    rec_rslt += strlen(asr_result_key);
    int count = 0;
    while (isdigit(*(rec_rslt + count))) {
      count++;
    }
    char id[count + 1];
    char *str = id;
    strncpy(str, rec_rslt, count);
    id[count] = '\0';
    printf("\ncommand id = %s\n", id);
  } else {
    fprintf(stderr, "\nstrlen asr_result_key NULL\n");
  }
  rec_rslt = NULL;
}

void *timer_task(void) {
  do {
    usleep(1000 * 1000);
    pthread_mutex_lock(&mutex);
    timer_count--;
    if (timer_count < 0) {
      is_stop_listening = 1; //通知asr停止识别，但是没有让asr立即停止识别
    }
    pthread_mutex_unlock(&mutex);
    if (timer_count < -5) { // n秒内，计时器未重置，强制跳出，结束计时器
      break;
    }
    // printf("\ntimer_count = ：%d\n", timer_count);
  } while (!asr_break); // asr 跳出循环时，计时器结束循环
}

int start_recog() {
  pthread_mutex_init(&mutex, NULL);
  init_record();
  while (1) { //循环唤醒
    init_awaken();
    start_awaken();
    init_asr();
    start_asr();
  }
  return 0;
}

int start_recog_thread() {
  pthread_t recog_thread;
  return pthread_create(&recog_thread, NULL, start_recog, NULL);
}

int main(int argc, char const *argv[]) {
  start_recog_thread();
  while (1) {
    usleep(30 * 1000 * 1000);
  }
  return 0;
}
