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
#define SAMPLE_RATE_8K (8000)
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

// const char *get_audio_file(void);	//选择进行离线语法识别的语音文件
int build_grammar(UserData *udata);  //构建离线识别语法网络
int update_lexicon(UserData *udata); //更新离线识别语法词典
int run_asr(UserData *udata);        //进行离线语法识别
int init_asr(void);                  //进行初始化操作
int start_asr(void);                 //开始识别

void start(void (*recall)(void));
void init_awaken();    //初始化唤醒参数
void init_record();    //初始化录音参数
void get_record();     //获取录音
void get_asr_result(); //获取识别结果

// int cb_ivw_msg_proc(char *, int, int, int, void *, void *); //唤醒回调
pthread_t get_asr_result_thread;
int thread_asr_result_id = -1;
int rc;                     //录音缓存实际大小
int size;                   //录音缓存容量
char *buffer = NULL;        //录音缓存
snd_pcm_t *handle = NULL;   //录音句柄
volatile int is_awaken = 0; //唤醒跳出条件
volatile int is_asr = 0;
const char *session_id = NULL; //唤醒id
snd_pcm_uframes_t frames = NULL;
unsigned int val = 0;
char *GRAMMAR_FILE = "command.bnf";
int err_code = MSP_SUCCESS;
char sse_hints[128];

/*
唤醒回调
*/
int cb_ivw_msg_proc(const char *sessionID, int msg, int param1, int param2,
                    const void *info, void *userData) {
  switch (msg) {
  case MSP_IVW_MSG_ISR_RESULT: //唤醒识别出错消息
    fprintf(stderr, "\n\nMSP_IVW_MSG_ISR_RESULT result = %s\n\n", info);
    is_awaken = 1;
    break;
  case MSP_IVW_MSG_ERROR: //唤醒出错消息
    fprintf(stderr, "\n\nMSP_IVW_MSG_ERROR errCode = %d\n\n", param1);
    break;
  case MSP_IVW_MSG_WAKEUP: //唤醒成功消息
    fprintf(stderr, "\n\nMSP_IVW_MSG_WAKEUP result = %s\n\n", info);
    is_awaken = 1;
    break;
  default:
    fprintf(stderr, "\n\nGet recall from awaken\n\n");
    break;
    return 0;
  }
}

/*
初始化监听参数
*/
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

  /* 16000 bits/second sampling rate  */
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

初始化唤醒参数
*/
void init_awaken() {

  fprintf(stderr, "init_awakne \n");
  int ret = MSP_SUCCESS;
  // char *grammar_list = NULL;
  const char *lgi_param =
      "appid = 595c9d5c,engine_start = ivw,ivw_res_path "
      "=fo|res/ivw/wakeupresource.jet, work_dir = ."; //使用唤醒需要在此设置engine_start
                                                      //= ivw,ivw_res_path
                                                      //=fo|xxx/xx 启动唤醒引擎
  const char *session_begin_params = "ivw_threshold=0:-20,sst=oneshot";

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
void get_record() {
  if (buffer == NULL) {
    buffer = (char *)malloc(size);
  }
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
}
/*


*/
void start_awaken() {
  /*start record*/
  // long loops = 5000000 / val;
  fprintf(stderr, "start_awaken\n");
  buffer = (char *)malloc(size);
  int audio_stat = MSP_AUDIO_SAMPLE_FIRST;
  fprintf(stderr, "listening.. \n");
  while (!is_awaken) {
    // loops--;
    get_record();
    // rc = write(1, buffer, size);
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
    printf("构建语法成功！ 语法ID:%s\n", info);
    if (NULL != grm_data)
      snprintf(grm_data->grammar_id, MAX_GRAMMARID_LEN - 1, info);
  } else
    printf("构建语法失败！%d\n", ecode);

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
    printf("打开\"%s\"文件失败！[%s]\n", GRAMMAR_FILE, strerror(errno));
    return -1;
  }

  fseek(grm_file, 0, SEEK_END);
  grm_cnt_len = ftell(grm_file);
  fseek(grm_file, 0, SEEK_SET);

  grm_content = (char *)malloc(grm_cnt_len + 1);
  if (NULL == grm_content) {
    printf("内存分配失败!\n");
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
  fprintf(stderr, "init_asr\n");
  ret = MSPLogin(
      NULL, NULL,
      login_config); //第一个参数为用户名，第二个参数为密码，传NULL即可，第三个参数是登录参数
  if (MSP_SUCCESS != ret) {
    fprintf(stderr, "登录失败：%d\n", ret);
    exit(1);
  }

  memset(&asr_data, 0, sizeof(UserData));
  fprintf(stderr, "构建离线识别语法网络...\n");
  ret = build_grammar(
      &asr_data); //第一次使用某语法进行识别，需要先构建语法网络，获取语法ID，之后使用此语法进行识别，无需再次构建
  if (MSP_SUCCESS != ret) {
    fprintf(stderr, "构建语法调用失败！\n");
    exit(1);
  }
  while (1 != asr_data.build_fini)
    usleep(300 * 1000);
  if (MSP_SUCCESS != asr_data.errcode)
    exit(1);
  fprintf(stderr, "离线识别语法网络构建完成，开始识别...\n");
  return ret;
}

/*
开始识别
*/
int start_asr(void) {
  fprintf(stderr, "start_asr\n");
  int ret = 0;
  ret = run_asr(&asr_data);
  MSPLogout();
  return ret;
}

int run_asr(UserData *udata) {
  char asr_params[MAX_PARAMS_LEN] = {NULL};
  int ep_status = MSP_EP_LOOKING_FOR_SPEECH;
  int rec_status = MSP_REC_STATUS_INCOMPLETE;
  int errcode = -1;

  //离线语法识别参数设置
  snprintf(asr_params, MAX_PARAMS_LEN - 1, "engine_type = local, \
		asr_res_path = %s, sample_rate = %d, \
		grm_build_path = %s, local_grammar = %s, \
		result_type = xml, result_encoding = UTF-8, ",
           ASR_RES_PATH, SAMPLE_RATE_16K, GRM_BUILD_PATH, udata->grammar_id);
  session_id = QISRSessionBegin(NULL, asr_params, &errcode);
  fprintf(stderr, "%d\n", session_id);
  if (NULL == session_id) {
    exit(1);
  }
  int aud_stat = MSP_AUDIO_SAMPLE_FIRST;
  int count = 0;
  while (!is_asr) {
    get_record();
      fprintf(stderr, ">");
    // write(1, buffer, size);
    count++;
    if (count > 500) {
      break;
    }
    errcode = QISRAudioWrite(session_id, buffer, size, aud_stat, &ep_status,
                             &rec_status);
    if (MSP_SUCCESS != errcode) {
      exit(1);
    }
    aud_stat = MSP_AUDIO_SAMPLE_CONTINUE;
    if (thread_asr_result_id == -1) {
      thread_asr_result_id =
          pthread_create(&get_asr_result_thread, NULL, get_asr_result, NULL);
    }
    if (ep_status == MSP_EP_AFTER_SPEECH) {
      break;
    }
  }
  // //主动点击音频结束
  QISRAudioWrite(session_id, (const void *)NULL, 0, MSP_AUDIO_SAMPLE_LAST,
                 &ep_status, &rec_status);
  usleep(10 * 1000 * 1000);
  QISRSessionEnd(session_id, NULL);
  return errcode;
}

/*

*/
void get_asr_result() {
  long count = 0;
  int errcode = -1;
  const char *rec_rslt = NULL;
  int rss_status = MSP_REC_STATUS_INCOMPLETE;
  while (!is_asr) {
    //获取识别结果
    usleep(800 * 1000);
    if (MSP_REC_STATUS_COMPLETE != rss_status && MSP_SUCCESS == errcode) {
      rec_rslt = QISRGetResult(session_id, &rss_status, 0, &errcode);
    }
    fprintf(stderr, "\n===========%d==========%d====\n", count++, session_id);
    if (NULL != rec_rslt)
      fprintf(stderr, "%s\n\n", rec_rslt);
    else
      fprintf(stderr, "没有识别结果！\n\n");
  }
}
int main(int argc, char const *argv[]) {
  init_record();
  // init_awaken();
  // start_awaken();
  init_asr();
  start_asr();
  return 0;
}
