/*
** Copyright 2008, Google Inc.
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/
// NOTE VERSION_C
#define REVISION_C "7006.3."
#define LOG_NDEBUG 0

#define LOG_TAG "QualcommCameraHardware"
#include <utils/Log.h>

#include "QualcommCameraHardware.h"

#include <utils/threads.h>
#include <binder/MemoryHeapPmem.h>
#include <utils/String16.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#if HAVE_ANDROID_OS
#include <linux/android_pmem.h>
#endif
#include <linux/ioctl.h>
#include "raw2jpeg.h"

#define LIKELY(exp)   __builtin_expect(!!(exp), 1)
#define UNLIKELY(exp) __builtin_expect(!!(exp), 0)

extern "C" {
#include "exifwriter.h"

#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <assert.h>
#include <stdlib.h>
#include <ctype.h>
#include <signal.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <stdlib.h>
#include <poll.h>

#include "msm_camera.h" // Cliq XT kernel

#define REVISION "0.4"

// init for CliqXT 
#define THUMBNAIL_WIDTH_STR   "192"
#define THUMBNAIL_HEIGHT_STR  "144"
// if not set, set them to the following
#define THUMBNAIL_WIDTH        192
#define THUMBNAIL_HEIGHT       144

// actual px for snapshoting
#define DEFAULT_PICTURE_WIDTH  2560
#define DEFAULT_PICTURE_HEIGHT 1920

#define THUMBNAIL_BUFFER_SIZE (THUMBNAIL_WIDTH * THUMBNAIL_HEIGHT * 3/2)
#define DEFAULT_PREVIEW_SETTING 2
#define DEFAULT_FRAMERATE 15
#define PREVIEW_SIZE_COUNT (sizeof(preview_sizes)/sizeof(preview_size_type))
#define NOT_FOUND -1
#define LOG_PREVIEW false

#if DLOPEN_LIBMMCAMERA
#include <dlfcn.h>

void* (*LINK_cam_conf)(void *data);
void* (*LINK_cam_frame)(void *data);
bool  (*LINK_jpeg_encoder_init)();
void  (*LINK_jpeg_encoder_join)();
unsigned char (*LINK_jpeg_encoder_encode)(const char* file_name, const cam_ctrl_dimension_t *dimen,
                                  const unsigned char* thumbnailbuf, int thumbnailfd,
                                  const unsigned char* snapshotbuf, int snapshotfd,
                                  common_crop_t *cropInfo);
int  (*LINK_camframe_terminate)();
void (*LINK_cam_set_frame_callback)();
//bool (*LINK_cam_release_frame)();
int8_t (*LINK_jpeg_encoder_setMainImageQuality)(uint32_t quality);
int8_t (*LINK_jpeg_encoder_setThumbnailQuality)(uint32_t quality);
int8_t (*LINK_jpeg_encoder_setRotation)(uint32_t rotation);
int8_t (*LINK_jpeg_encoder_setLocation)(const camera_position_type *location);
//
// callbacks
void  (**LINK_mmcamera_camframe_callback)(struct msm_frame_t *frame);
void  (**LINK_mmcamera_jpegfragment_callback)(uint8_t *buff_ptr,
                                              uint32_t buff_size);
void  (**LINK_mmcamera_jpeg_callback)(jpeg_event_t status);
#else
#define LINK_cam_conf cam_conf
#define LINK_cam_frame cam_frame
#define LINK_jpeg_encoder_init jpeg_encoder_init
#define LINK_jpeg_encoder_join jpeg_encoder_join
#define LINK_jpeg_encoder_encode jpeg_encoder_encode
#define LINK_camframe_terminate camframe_terminate
#define LINK_cam_set_frame_callback cam_set_frame_callback
//#define LINK_cam_release_frame cam_release_frame
#define LINK_jpeg_encoder_setMainImageQuality jpeg_encoder_setMainImageQuality
#define LINK_jpeg_encoder_setThumbnailQuality jpeg_encoder_setThumbnailQuality
#define LINK_jpeg_encoder_setRotation jpeg_encoder_setRotation
#define LINK_jpeg_encoder_setLocation jpeg_encoder_setLocation
extern void (*mmcamera_camframe_callback)(struct msm_frame_t *frame);
extern void (*mmcamera_jpegfragment_callback)(uint8_t *buff_ptr,
                                      uint32_t buff_size);
extern void (*mmcamera_jpeg_callback)(jpeg_event_t status);
#endif

} // extern "C"

struct preview_size_type {
    int width;
    int height;
};

static preview_size_type preview_sizes[] = {
// 	{ 800, 480 }, //WVGA
// 	{ 640, 480 }, //VGA
    { 480, 320 }, // HVGA
    { 352, 288 }, // CIF
    { 320, 240 }, // QVGA
    { 176, 144 }, // QCIF
};

static int attr_lookup(const struct str_map *const arr, const char *name)
{
    if (name) {
        const struct str_map *trav = arr;
        while (trav->desc) {
            if (!strcmp(trav->desc, name))
                return trav->val;
            trav++;
        }
    }
    return NOT_FOUND;
}

#define INIT_VALUES_FOR(parm) do {                               \
    if (!parm##_values) {                                        \
        parm##_values = (char *)malloc(sizeof(parm)/             \
                                       sizeof(parm[0])*30);      \
        char *ptr = parm##_values;                               \
        const str_map *trav;                                     \
        for (trav = parm; trav->desc; trav++) {                  \
            int len = strlen(trav->desc);                        \
            strcpy(ptr, trav->desc);                             \
            ptr += len;                                          \
            *ptr++ = ',';                                        \
        }                                                        \
        *--ptr = 0;                                              \
    }                                                            \
} while(0)

// from aeecamera.h
static const str_map whitebalance[] = {
    { "minus1",       CAMERA_WB_MIN_MINUS_1 },
    { "auto",         CAMERA_WB_AUTO },
    { "custom",       CAMERA_WB_CUSTOM },
    { "incandescent", CAMERA_WB_INCANDESCENT },
    { "fluorescent",  CAMERA_WB_FLUORESCENT },
    { "daylight",     CAMERA_WB_DAYLIGHT },
    { "cloudy",       CAMERA_WB_CLOUDY_DAYLIGHT },
    { "twilight",     CAMERA_WB_TWILIGHT },
    { "shade",        CAMERA_WB_SHADE },
    { "maxplus1",     CAMERA_WB_MAX_PLUS_1 },
    { NULL, 0 }
};
static char *whitebalance_values;

// from camera_effect_t
static const str_map effect[] = {
    { "minus1",     0 },  /* This list must match aeecamera.h */
    { "none",       CAMERA_EFFECT_OFF },  //CAMERA_EFFECT_OFF
    { "b/w",        CAMERA_EFFECT_MONO },
    { "negative",   CAMERA_EFFECT_NEGATIVE },
    { "solarize",   CAMERA_EFFECT_SOLARIZE },
    { "pastel",     5 /*CAMERA_EFFECT_PASTEL*/ },
    { "mosaic",     6 /*CAMERA_EFFECT_MOSAIC*/ },
    { "resize",     7 /*CAMERA_EFFECT_RESIZE*/ },
    { "sepia",      CAMERA_EFFECT_SEPIA },
    { "posterize",  CAMERA_EFFECT_POSTERIZE },
    { "whiteboard", CAMERA_EFFECT_WHITEBOARD },
    { "blackboard", CAMERA_EFFECT_BLACKBOARD },
    { "aqua",       CAMERA_EFFECT_AQUA },
    { "masplus",    13 /*CAMERA_EFFECT_MAX_PLUS_1*/ },
    { NULL, 0 }
};
static char *effect_values;

// from qcamera/common/camera.h
static const str_map antibanding[] = {
    { "off",   CAMERA_ANTIBANDING_OFF },
    { "60hz",  CAMERA_ANTIBANDING_60HZ },
    { "50hz",  CAMERA_ANTIBANDING_50HZ },
    { "auto",  CAMERA_ANTIBANDING_AUTO },
    { "max",   4 /*CAMERA_ANTIBANDING_MAX*/ },
    { NULL, 0 }
};
static char *antibanding_values;

static const str_map flashmode[] = {
    { "off", LED_MODE_OFF },
    { "auto", LED_MODE_AUTO },
    { "on",  LED_MODE_ON },
    { "torch", LED_MODE_TORCH },
    { NULL, 0 }
};
static char *flashmode_values;

static const str_map picturesize[] = {
	    { "2560x1920", 0 },// max res, cannot zoom
	    { "2048x1536", 2 },// 2 ok, 3 broken
	    { "1600x1200", 4 },// 4 ok, 5 broken
	    { "1280x960", 6 }, // 6 ok, 7 broken
	    { "640x480", 11 }, // 11 ok, 12 broken
	    { "320x240", 11 }, // 11 ok, 12 broken
	    { "352x288", 10 }, // 10 ok, 11 broken
	    { "176x144", 16 }, // 16 ok, 17 broken
	    { NULL, 0 }
};

static char *picturesize_values;

// round to the next power of two
static inline unsigned clp2(unsigned x)
{
    x = x - 1;
    x = x | (x >> 1);
    x = x | (x >> 2);
    x = x | (x >> 4);
    x = x | (x >> 8);
    x = x | (x >>16);
    return x + 1;
}

namespace android {

static Mutex singleton_lock;
static bool singleton_releasing;
static Condition singleton_wait;

static void receive_camframe_callback(struct msm_frame_t *frame);
static void receive_jpeg_fragment_callback(uint8_t *buff_ptr, uint32_t buff_size);
static void receive_jpeg_callback(jpeg_event_t status);

static int camerafd;
static int fd_frame;
struct msm_frame_t *frameA;
bool bFramePresent;
pthread_t w_thread;
pthread_t jpegThread;
//Zoom
static int32_t mMaxZoom = -1;
static bool zoomSupported = false;

void *opencamerafd(void *arg) {
    camerafd = open(MSM_CAMERA_CONTROL, O_RDWR);
    if (camerafd < 0)
        LOGE("Camera control %s open failed: %s!", MSM_CAMERA_CONTROL, strerror(errno));
    else
        LOGV("opening %s fd: %d", MSM_CAMERA_CONTROL, camerafd);

    return NULL;
}

QualcommCameraHardware::QualcommCameraHardware()
    : mParameters(),
      mPreviewHeight(-1),
      mPreviewWidth(-1),
      mRawHeight(-1),
      mRawWidth(-1),
      mCameraRunning(false),
      mPreviewInitialized(false),
      mRawInitialized(false),
      mFrameThreadRunning(false),
      mSnapshotThreadRunning(false),
      mReleasedRecordingFrame(false),
      mNotifyCb(0),
      mDataCb(0),
      mDataCbTimestamp(0),
      mCallbackCookie(0),
      mMsgEnabled(0),
      mPreviewFrameSize(0),
      mRawSize(0),
      mCameraControlFd(-1),
      mAutoFocusThreadRunning(false),
      mAutoFocusFd(-1),
      mInPreviewCallback(false),
      mCameraRecording(false),
      mCurZoom(0)
{
    LOGV("constructor E");
    if((pthread_create(&w_thread, NULL, opencamerafd, NULL)) != 0) {
        LOGE("Camera open thread creation failed");
    }
    memset(&mDimension, 0, sizeof(mDimension));
    memset(&mCrop, 0, sizeof(mCrop));
    LOGV("constructor X");
}

void QualcommCameraHardware::initDefaultParameters()
{
    CameraParameters p;

    LOGV("initDefaultParameters E");

    preview_size_type *ps = &preview_sizes[DEFAULT_PREVIEW_SETTING];
    p.setPreviewSize(ps->width, ps->height);
    p.setPreviewFrameRate(DEFAULT_FRAMERATE);
    p.setPreviewFormat(CameraParameters::PIXEL_FORMAT_YUV420SP); // informative
    p.setPictureFormat(CameraParameters::PIXEL_FORMAT_JPEG); // informative

    p.set("jpeg-quality", "100"); // maximum quality
    p.set("jpeg-thumbnail-width", THUMBNAIL_WIDTH_STR); // informative
    p.set("jpeg-thumbnail-height", THUMBNAIL_HEIGHT_STR); // informative
    p.set("jpeg-thumbnail-quality", "85");

    p.setPictureSize(DEFAULT_PICTURE_WIDTH, DEFAULT_PICTURE_HEIGHT);
    p.set(CameraParameters::KEY_ANTIBANDING, CameraParameters::ANTIBANDING_OFF);
    p.set(CameraParameters::KEY_EFFECT, CameraParameters::EFFECT_NONE);
    p.set(CameraParameters::KEY_WHITE_BALANCE, CameraParameters::WHITE_BALANCE_AUTO);
    p.set(CameraParameters::KEY_FLASH_MODE, CameraParameters::FLASH_MODE_AUTO);

    // Zoom parameters
    p.set(CameraParameters::KEY_ZOOM_SUPPORTED, "true");
    p.set(CameraParameters::KEY_ZOOM, "0");
    p.set(CameraParameters::KEY_MAX_ZOOM, 5);
    p.set(CameraParameters::KEY_ZOOM_RATIOS, "100,150,175,200,250,300");

    // This will happen only once in the lifetime of the mediaserver process.
    // We do not free the _values arrays when we destroy the camera object.
    INIT_VALUES_FOR(antibanding);
    INIT_VALUES_FOR(effect);
    INIT_VALUES_FOR(whitebalance);
    INIT_VALUES_FOR(flashmode);
    INIT_VALUES_FOR(picturesize);

    p.set(CameraParameters::KEY_SUPPORTED_ANTIBANDING, antibanding_values);
    p.set(CameraParameters::KEY_SUPPORTED_EFFECTS, effect_values);
    p.set(CameraParameters::KEY_SUPPORTED_WHITE_BALANCE, whitebalance_values);
    p.set(CameraParameters::KEY_SUPPORTED_FLASH_MODES, flashmode_values);
    p.set(CameraParameters::KEY_SUPPORTED_PICTURE_SIZES, "320x240,640x480,1280x960,1600x1200,2048x1536,2560x1920");
    p.set(CameraParameters::KEY_SUPPORTED_PREVIEW_SIZES, "176x144,320x240,352x288,480x320");

    if (setParameters(p) != NO_ERROR) {
        LOGE("Failed to set default parameters?!");
    }

    LOGV("initDefaultParameters X");

}

void QualcommCameraHardware::setCallbacks(notify_callback notify_cb,
                                      data_callback data_cb,
                                      data_callback_timestamp data_cb_timestamp,
                                      void* user)
{
    Mutex::Autolock lock(mLock);
    mNotifyCb = notify_cb;
    mDataCb = data_cb;
    mDataCbTimestamp = data_cb_timestamp;
    mCallbackCookie = user;
}

void QualcommCameraHardware::enableMsgType(int32_t msgType)
{
    Mutex::Autolock lock(mLock);
    LOGV("enableMsgType(%d)", msgType);
    mMsgEnabled |= msgType;
}

void QualcommCameraHardware::disableMsgType(int32_t msgType)
{
    Mutex::Autolock lock(mLock);
    LOGD("DisableMsgType( %d )", msgType);
    mMsgEnabled &= ~msgType;
}

bool QualcommCameraHardware::msgTypeEnabled(int32_t msgType)
{
    Mutex::Autolock lock(mLock);
    LOGD("msgTypeEnabled( %d )", msgType);
    return (mMsgEnabled & msgType);
}


#define ROUND_TO_PAGE(x)  (((x)+0xfff)&~0xfff)

void QualcommCameraHardware::startCamera()
{
    LOGV("startCamera E");

    libmmcamera = ::dlopen("libmmcamera.so", RTLD_NOW);
    LOGV("loading libmmcamera at %p", libmmcamera);
    if (!libmmcamera) {
        LOGE("FATAL ERROR: could not dlopen libmmcamera.so: %s", dlerror());
        return;
    }

    libmmcamera_target = ::dlopen("libmm-qcamera-tgt.so", RTLD_NOW);
    LOGV("loading libmm-qcamera-tgt at %p", libmmcamera_target);
    if (!libmmcamera_target) {
        LOGE("FATAL ERROR: could not dlopen libmm-qcamera_target.so: %s", dlerror());
        return;
    }

#if 0 // useless now
    *(void **)&LINK_cam_frame =
        ::dlsym(libmmcamera, "cam_frame");
#endif

    *(void **)&LINK_camframe_terminate =
        ::dlsym(libmmcamera, "camframe_terminate");

    *(void **)&LINK_jpeg_encoder_init =
        ::dlsym(libmmcamera, "jpeg_encoder_init");

    *(void **)&LINK_jpeg_encoder_encode =
        ::dlsym(libmmcamera, "jpeg_encoder_encode");

    *(void **)&LINK_jpeg_encoder_join =
        ::dlsym(libmmcamera, "jpeg_encoder_join");

    *(void **)&LINK_mmcamera_camframe_callback =
        ::dlsym(libmmcamera, "camframe_callback");

    *LINK_mmcamera_camframe_callback = receive_camframe_callback;

    // firesnatch !! this doesn't exist in motorola code
    //*(void **)&LINK_cam_set_frame_callback =
    //    ::dlsym(libmmcamera, "cam_set_frame_callback");
    
    // firesnatch !! this doesn't exist in motorola code
    //*(void **)&LINK_cam_release_frame =
    //    ::dlsym(libmmcamera, "cam_release_frame");

    *(void **)&LINK_mmcamera_jpegfragment_callback =
        ::dlsym(libmmcamera, "jpegfragment_callback");

    *LINK_mmcamera_jpegfragment_callback = receive_jpeg_fragment_callback;

    *(void **)&LINK_mmcamera_jpeg_callback =
        ::dlsym(libmmcamera, "jpeg_callback");

    *LINK_mmcamera_jpeg_callback = receive_jpeg_callback;

    *(void**)&LINK_jpeg_encoder_setMainImageQuality =
        ::dlsym(libmmcamera, "jpeg_encoder_setMainImageQuality");

    *(void **)&LINK_cam_conf =
        ::dlsym(libmmcamera_target, "cam_conf");

    /* The control thread is in libcamera itself. */
    LOGV("pthread_join on control thread");
    if (pthread_join(w_thread, NULL) != 0) {
        LOGE("Camera open thread exit failed");
        return;
    }

    mCameraControlFd = camerafd;

    // maitain a fd for select() later
    fd_frame = open(MSM_CAMERA_CONTROL, O_RDWR);
    if (fd_frame < 0)
        LOGE("cam_frame_click: cannot open %s: %s",
             MSM_CAMERA_CONTROL, strerror(errno));

    if (!LINK_jpeg_encoder_init()) {
        LOGE("jpeg_encoding_init failed.");
    }

    if ((pthread_create(&mCamConfigThread, NULL, LINK_cam_conf, NULL)) != 0)
        LOGE("Config thread creation failed!");
    else
        LOGV("Config thread created successfully");

    bFramePresent=false;

    LOGV("startCamera X");
}

status_t QualcommCameraHardware::dump(int fd,
                                      const Vector<String16>& args) const
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    // Dump internal primitives.
    result.append("QualcommCameraHardware::dump");
    snprintf(buffer, 255, "preview width(%d) x height (%d)\n",
             mPreviewWidth, mPreviewHeight);
    result.append(buffer);
    snprintf(buffer, 255, "raw width(%d) x height (%d)\n",
             mRawWidth, mRawHeight);
    result.append(buffer);
    snprintf(buffer, 255,
             "preview frame size(%d), raw size (%d), jpeg size (%d) "
             "and jpeg max size (%d)\n", mPreviewFrameSize, mRawSize,
             mJpegSize, mJpegMaxSize);
    result.append(buffer);
    write(fd, result.string(), result.size());

    // Dump internal objects.
    if (mPreviewHeap != 0) {
        mPreviewHeap->dump(fd, args);
    }
    if (mRawHeap != 0) {
        mRawHeap->dump(fd, args);
    }
    if (mJpegHeap != 0) {
        mJpegHeap->dump(fd, args);
    }
    mParameters.dump(fd, args);
    return NO_ERROR;
}

bool QualcommCameraHardware::reg_unreg_buf(int camfd,
                                           int width,
                                           int height,
                                           msm_frame_t *frame,
                                           msm_pmem_t pmem_type,
                                           unsigned char unregister,
                                           unsigned char active)
{
    uint32_t y_size;
    struct msm_pmem_info_t pmemBuf;
    uint32_t ioctl_cmd;
    int ioctlRetVal;

    memset(&pmemBuf, 0, sizeof(pmemBuf));

    pmemBuf.type     = pmem_type;
    pmemBuf.fd       = frame->fd;
    pmemBuf.vaddr    = (unsigned long *)frame->buffer;
    pmemBuf.y_off    = (frame->y_off + 3) & ~3; // aligned to 4
    pmemBuf.cbcr_off = (frame->cbcr_off + 3) & ~3;
    pmemBuf.active   = active;

    ioctl_cmd = unregister ?
                MSM_CAM_IOCTL_UNREGISTER_PMEM :
                MSM_CAM_IOCTL_REGISTER_PMEM;

    if ((ioctlRetVal = ioctl(camfd, ioctl_cmd, &pmemBuf)) < 0) {
        LOGE("reg_unreg_buf: MSM_CAM_IOCTL_(UN)REGISTER_PMEM ioctl failed %d",
            ioctlRetVal);
        return false;
    }

    return true;
}

void QualcommCameraHardware::native_register_preview_bufs(
    int camfd,
    void *pDim,
    struct msm_frame_t *frame,
    unsigned char active)
{
    cam_ctrl_dimension_t *dimension = (cam_ctrl_dimension_t *)pDim;

    reg_unreg_buf(camfd,
                  dimension->display_width,
                  dimension->display_height,
                  frame,
                  MSM_PMEM_OUTPUT2,
                  false,
                  active);
}

void QualcommCameraHardware::native_unregister_preview_bufs(
    int camfd,
    void *pDim,
    struct msm_frame_t *frame)
{
    cam_ctrl_dimension_t *dimension = (cam_ctrl_dimension_t *)pDim;

    reg_unreg_buf(camfd,
                  dimension->display_width,
                  dimension->display_height,
                  frame,
                  MSM_PMEM_OUTPUT2,
                  true,
                  true);
}

static bool native_get_maxzoom(int camfd, void *pZm)
{
    LOGV("native_get_maxzoom E");

    struct msm_ctrl_cmd_t ctrlCmd;
    int32_t *pZoom = (int32_t *)pZm;

    ctrlCmd.type       = CAMERA_GET_PARM_MAXZOOM;
    ctrlCmd.timeout_ms = 5000;
    ctrlCmd.length     = sizeof(int32_t);
    ctrlCmd.value      = pZoom;

    if (ioctl(camfd, MSM_CAM_IOCTL_CTRL_COMMAND, &ctrlCmd) < 0) {
        LOGE("native_get_maxzoom: ioctl fd %d error %s", camfd, strerror(errno));
        return false;
    }

    LOGV("MaxZoom value is %d", *(int32_t *)ctrlCmd.value);
    memcpy(pZoom, (int32_t *)ctrlCmd.value, sizeof(int32_t));

    LOGV("native_get_maxzoom X");
    return true;
}

static bool native_set_afmode(int camfd, isp3a_af_mode_t af_type)
{
    struct msm_ctrl_cmd_t ctrlCmd;

    ctrlCmd.timeout_ms = 5000;
    ctrlCmd.type       = CAMERA_SET_PARM_AUTO_FOCUS;
    ctrlCmd.length     = sizeof(int);
    ctrlCmd.value      = (void *) &af_type;

    if (ioctl(camfd, MSM_CAM_IOCTL_CTRL_COMMAND, &ctrlCmd) < 0) {
        LOGE("native_set_afmode: MSM_CAM_IOCTL_CTRL_COMMAND fd %d error %s",
             camfd,
             strerror(errno));
        return false;
    }

    return true;
}

static int native_get_af_result(int camfd) {
    int ret;
    struct msm_ctrl_cmd_t ctrlCmd;

    ctrlCmd.timeout_ms = 5000;
    ctrlCmd.type = 0;
    ctrlCmd.length = 0;
    ctrlCmd.value = NULL;
    ctrlCmd.status = 0;

    errno = 0;

    ret = ioctl(camfd, MSM_CAM_IOCTL_AF_CTRL, &ctrlCmd);

    if (ret < 0) {
        if (errno == ETIMEDOUT)
            LOGE("native_get_af_result: ioctl timedout\n");
        else
            LOGE("native_get_af_result: ioctl failed. ioctl return %d, errno = %d\n", ret, errno);

        return 1;
    }

    LOGV("af_result: %d\n", ctrlCmd.status);
    if (ctrlCmd.status == 1)
        return 0; // focus passed
    else
        return 1; // focus failed
}


// need to snapshot
static bool native_cancel_afmode(int camfd, int af_fd)
{
    struct msm_ctrl_cmd_t ctrlCmd;

    ctrlCmd.timeout_ms = 5000;
    ctrlCmd.type       = CAMERA_AUTO_FOCUS_CANCEL;
    ctrlCmd.length     = 0;
    ctrlCmd.value      = NULL;

    if (ioctl(camfd, MSM_CAM_IOCTL_CTRL_COMMAND, &ctrlCmd) < 0) {
        LOGE("native_cancel_afmode: MSM_CAM_IOCTL_CTRL_COMMAND fd %d error %s",
             camfd,
             strerror(errno));
        return false;
    }

    return true;
}

static bool native_start_preview(int camfd)
{
    struct msm_ctrl_cmd_t ctrlCmd;

    ctrlCmd.timeout_ms = 5000;
    ctrlCmd.type       = CAMERA_START_PREVIEW;
    ctrlCmd.length     = 0;
    ctrlCmd.value      = NULL;

    if (ioctl(camfd, MSM_CAM_IOCTL_CTRL_COMMAND, &ctrlCmd) < 0) {
        LOGE("native_start_preview: MSM_CAM_IOCTL_CTRL_COMMAND fd %d error %s",
             camfd,
             strerror(errno));
        return false;
    }

    return true;
}

static bool native_get_picture(int camfd, common_crop_t *crop)
{
    LOGV("native_get_picture E");
    struct msm_ctrl_cmd_t ctrlCmd;

    ctrlCmd.timeout_ms = 5000;
    ctrlCmd.length     = sizeof(common_crop_t);
    ctrlCmd.value      = crop;

    if(ioctl(camfd, MSM_CAM_IOCTL_GET_PICTURE, &ctrlCmd) < 0) {
        LOGE("native_get_picture: MSM_CAM_IOCTL_GET_PICTURE fd %d error %s",
             camfd,
             strerror(errno));
        return false;
    }

    LOGV("crop: in1_w %d", crop->in1_w);
    LOGV("crop: in1_h %d", crop->in1_h);
    LOGV("crop: out1_w %d", crop->out1_w);
    LOGV("crop: out1_h %d", crop->out1_h);

    LOGV("crop: in2_w %d", crop->in2_w);
    LOGV("crop: in2_h %d", crop->in2_h);
    LOGV("crop: out2_w %d", crop->out2_w);
    LOGV("crop: out2_h %d", crop->out2_h);

    LOGV("crop: update %d", crop->update_flag);

    LOGV("native_get_picture status after ioctl %d", ctrlCmd.status);
    LOGV("native_get_picture X");

    return true;
}

static bool native_stop_preview(int camfd)
{
    struct msm_ctrl_cmd_t ctrlCmd;
    ctrlCmd.timeout_ms = 5000;
    ctrlCmd.type       = CAMERA_STOP_PREVIEW;
    ctrlCmd.length     = 0;

    if(ioctl(camfd, MSM_CAM_IOCTL_CTRL_COMMAND, &ctrlCmd) < 0) {
        LOGE("native_stop_preview: ioctl fd %d error %s",
             camfd,
             strerror(errno));
        return false;
    }

    return true;
}

static bool native_start_snapshot(int camfd)
{
    struct msm_ctrl_cmd_t ctrlCmd;

    ctrlCmd.timeout_ms = 5000;
    ctrlCmd.type       = CAMERA_START_SNAPSHOT;
    ctrlCmd.length     = 0;

    if(ioctl(camfd, MSM_CAM_IOCTL_CTRL_COMMAND, &ctrlCmd) < 0) {
        LOGE("native_start_snapshot: ioctl fd %d error %s",
             camfd,
             strerror(errno));
        return false;
    }

    return true;
}

static bool native_stop_snapshot (int camfd)
{
    struct msm_ctrl_cmd_t ctrlCmd;

    ctrlCmd.timeout_ms = 5000;
    ctrlCmd.type       = CAMERA_STOP_SNAPSHOT;
    ctrlCmd.length     = 0;

    if (ioctl(camfd, MSM_CAM_IOCTL_CTRL_COMMAND, &ctrlCmd) < 0) {
        LOGE("native_stop_snapshot: ioctl fd %d error %s",
             camfd,
             strerror(errno));
        return false;
    }

    return true;
}

void *jpeg_encoder_thread( void *user )
{
    LOGD("jpeg_encoder_thread E");

    sp<QualcommCameraHardware> obj = QualcommCameraHardware::getInstance();
    if (obj != 0) {
        obj->runJpegEncodeThread(user);
    }
    else LOGW("not starting frame thread: the object went away!");

    LOGD("jpeg_encoder_thread X");

    return NULL;
}

static bool mJpegThreadRunning = false;
bool QualcommCameraHardware::native_jpeg_encode(void)
{
    int jpeg_quality = mParameters.getInt("jpeg-quality");
    if (jpeg_quality >= 0) {
        LOGV("native_jpeg_encode, current jpeg main img quality = %d",
             jpeg_quality);
        if(!LINK_jpeg_encoder_setMainImageQuality(jpeg_quality)) {
            LOGE("native_jpeg_encode set jpeg-quality failed");
            return false;
        }
        LOGV("jpeg main img quality done");

    }

    int thumbnail_quality = mParameters.getInt("jpeg-thumbnail-quality");
    if (thumbnail_quality >= 0) {
        LOGV("native_jpeg_encode, current jpeg thumbnail quality = %d",
             thumbnail_quality);
    }

    int rotation = mParameters.getInt("rotation");
    if (rotation >= 0) {
        LOGV("native_jpeg_encode, rotation = %d", rotation);
    }

    mDimension.filler7 = 2560;
    mDimension.filler8 = 1920;

    int ret = !pthread_create(&jpegThread,
                              NULL,
                              jpeg_encoder_thread,
                              NULL);
    if (ret)
        mJpegThreadRunning = true;

    return true;
}

bool QualcommCameraHardware::native_set_dimension(cam_ctrl_dimension_t *value)
{
    LOGV("native_set_dimension: EX");
    return native_set_parm(CAMERA_SET_PARM_DIMENSION,
                           sizeof(cam_ctrl_dimension_t), value);
}

bool QualcommCameraHardware::native_set_parm(
    cam_ctrl_type type, uint16_t length, void *value)
{
    int rc = true;
    struct msm_ctrl_cmd_t ctrlCmd;

    ctrlCmd.timeout_ms = 5000;
    ctrlCmd.type       = (uint16_t)type;
    ctrlCmd.length     = length;
    ctrlCmd.value      = value;

    LOGV("native_set_parm: type: %d, length=%d", type, length);

    rc = ioctl(mCameraControlFd, MSM_CAM_IOCTL_CTRL_COMMAND, &ctrlCmd);
    if(rc < 0 || ctrlCmd.status != CAM_CTRL_SUCCESS) {
        LOGE("ioctl error. camfd=%d, type=%d, length=%d, rc=%d, ctrlCmd.status=%d, %s",
             mCameraControlFd, type, length, rc, ctrlCmd.status, strerror(errno));
        return false;
    }
    return true;
}

void QualcommCameraHardware::jpeg_set_location()
{
    bool encode_location = true;
    camera_position_type pt;

#define PARSE_LOCATION(what,type,fmt,desc) do {                                \
        pt.what = 0;                                                           \
        const char *what##_str = mParameters.get("gps-"#what);                 \
        LOGV("GPS PARM %s --> [%s]", "gps-"#what, what##_str);                 \
        if (what##_str) {                                                      \
            type what = 0;                                                     \
            if (sscanf(what##_str, fmt, &what) == 1)                           \
                pt.what = what;                                                \
            else {                                                             \
                LOGE("GPS " #what " %s could not"                              \
                     " be parsed as a " #desc, what##_str);                    \
                encode_location = false;                                       \
            }                                                                  \
        }                                                                      \
        else {                                                                 \
            LOGV("GPS " #what " not specified: "                               \
                 "defaulting to zero in EXIF header.");                        \
            encode_location = false;                                           \
       }                                                                       \
    } while(0)

    PARSE_LOCATION(timestamp, long, "%ld", "long");
    if (!pt.timestamp) pt.timestamp = time(NULL);
    PARSE_LOCATION(altitude, short, "%hd", "short");
    PARSE_LOCATION(latitude, double, "%lf", "double float");
    PARSE_LOCATION(longitude, double, "%lf", "double float");

#undef PARSE_LOCATION

    if (encode_location) {
        LOGD("setting image location ALT %d LAT %lf LON %lf",
             pt.altitude, pt.latitude, pt.longitude);
    }
    else LOGV("not setting image location");
}

static void handler(int sig, siginfo_t *siginfo, void *context)
{
    pthread_exit(NULL);
}

// Set transfer preview meanwhile new preview is captured
static void *prev_frame_click(void *user)
{
    while (true) {
        usleep(100);
        if(bFramePresent) {
            if(LOG_PREVIEW)
                LOGV("PREVIEW ARRIVED !!!!!!");
            receive_camframe_callback(frameA);
            bFramePresent=false;
        }
    }
    return NULL;
}

// customized cam_frame function based on reassembled libmmcamera.so
static void *cam_frame_click(void *data)
{
    LOGV("Entering cam_frame_click");

    frameA = (msm_frame_t *)data;

    struct sigaction act;

    pthread_mutex_t mutex_camframe = PTHREAD_MUTEX_INITIALIZER;
    struct timeval timeout;
    fd_set readfds;
    int ret;

    // found in assembled codes of all libmmcamera
    memset(&readfds, 0, sizeof(readfds));

    act.sa_sigaction = &handler;
    act.sa_flags = SA_SIGINFO;
    if (sigaction(SIGUSR1, &act, NULL) != 0) {
        LOGE("sigaction in cam_frame failed");
        pthread_exit(NULL);
    }

    FD_ZERO(&readfds);
    FD_SET(fd_frame, &readfds);

    while (true) {
        timeout.tv_sec = 1; // This is not important JUST TIMEOUT for fail
        timeout.tv_usec = 0;

        ret = select(fd_frame+1, &readfds, NULL, NULL, &timeout);
        if (ret == -1) {
            LOGE("calling select failed!");
            break;
        } else if (FD_ISSET(fd_frame, &readfds)) {
            pthread_mutex_lock(&mutex_camframe);
            // ready to get frame
            ret = ioctl(fd_frame, MSM_CAM_IOCTL_GETFRAME, frameA);
            if (ret >= 0) {
                // put buffers to config VFE
                if (ioctl(fd_frame, MSM_CAM_IOCTL_RELEASE_FRAME_BUFFER, frameA) < 0)
                    LOGE("MSM_CAM_IOCTL_RELEASE_FRAME_BUFFER error %s", strerror(errno));
                else
                    bFramePresent=true;
            } else
                LOGE("MSM_CAM_IOCTL_GETFRAME error %s", strerror(errno));
            pthread_mutex_unlock(&mutex_camframe);
        } else {
            LOGV("frame is not ready! select returns %d", ret);
            usleep(100000);
        }
    }

    return NULL;
}

#if 0
void QualcommCameraHardware::runFrameThread(void *data)
{
    LOGV("runFrameThread E");

#if DLOPEN_LIBMMCAMERA
    // We need to maintain a reference to libmmcamera.so for the duration of the
    // frame thread, because we do not know when it will exit relative to the
    // lifetime of this object.  We do not want to dlclose() libmmcamera while
    // LINK_cam_frame is still running.
    void *libhandle = ::dlopen("libmmcamera.so", RTLD_NOW);
    LOGV("FRAME: loading libmmcamera at %p", libhandle);
    if (!libhandle) {
        LOGE("FATAL ERROR: could not dlopen libmmcamera.so: %s", dlerror());
    }
    if (libhandle)
#endif
    {
        cam_frame_click((msm_frame_t *)data);
    }

#if DLOPEN_LIBMMCAMERA
    if (libhandle) {
        ::dlclose(libhandle);
        LOGV("FRAME: dlclose(libmmcamera)");
    }
#endif

    mFrameThreadWaitLock.lock();
    mFrameThreadRunning = false;
    mFrameThreadWait.signal();
    mFrameThreadWaitLock.unlock();

    LOGV("runFrameThread X");
}

void *frame_thread(void *user)
{
    LOGV("frame_thread E");
    sp<QualcommCameraHardware> obj = QualcommCameraHardware::getInstance();
    if (obj != 0) {
        obj->runFrameThread(user);
    }
    else LOGW("not starting frame thread: the object went away!");
    LOGV("frame_thread X");
    return NULL;
}
#endif

void QualcommCameraHardware::runJpegEncodeThread(void *data)
{
    unsigned char *buffer ;

    int rotation = mParameters.getInt("rotation");
    LOGD("native_jpeg_encode, rotation = %d", rotation);

    bool encode_location = true;
        camera_position_type pt;

        #define PARSE_LOCATION(what,type,fmt,desc) do { \
                pt.what = 0; \
                const char *what##_str = mParameters.get("gps-"#what); \
                LOGD("GPS PARM %s --> [%s]", "gps-"#what, what##_str); \
                if (what##_str) { \
                        type what = 0; \
                        if (sscanf(what##_str, fmt, &what) == 1) \
                                pt.what = what; \
                        else { \
                                LOGE("GPS " #what " %s could not" \
                                " be parsed as a " #desc, what##_str); \
                                encode_location = false; \
                        } \
                } \
                else { \
                        LOGD("GPS " #what " not specified: " \
                        "defaulting to zero in EXIF header."); \
                        encode_location = false; \
                } \
        } while(0)

    PARSE_LOCATION(timestamp, long, "%ld", "long");
    if (!pt.timestamp) pt.timestamp = time(NULL);
    PARSE_LOCATION(altitude, short, "%hd", "short");
    PARSE_LOCATION(latitude, double, "%lf", "double float");
    PARSE_LOCATION(longitude, double, "%lf", "double float");

        #undef PARSE_LOCATION

    if (encode_location) {
        LOGD("setting image location ALT %d LAT %lf LON %lf",
             pt.altitude, pt.latitude, pt.longitude);
    }
    else {
        LOGV("not setting image location");
    }

    camera_position_type *npt = &pt ;
    if(!encode_location) {
        npt = NULL;
    }

    int jpeg_quality = mParameters.getInt("jpeg-quality");
    if (yuv420_save2jpeg((unsigned char*) mJpegHeap->mHeap->base(),
        mRawHeap->mHeap->base(), mRawWidth, mRawHeight, jpeg_quality, &mJpegSize))
        LOGV("jpegConvert done! ExifWriter...");
    else
        LOGE("jpegConvert failed!");

    writeExif(mJpegHeap->mHeap->base(), mJpegHeap->mHeap->base(), mJpegSize,
            &mJpegSize, rotation, npt);

    receiveJpegPicture();
}

bool QualcommCameraHardware::initPreview()
{
    // See comments in deinitPreview() for why we have to wait for the frame
    // thread here, and why we can't use pthread_join().
    LOGV("initPreview E: preview size=%dx%d", mPreviewWidth, mPreviewHeight);

    mFrameThreadWaitLock.lock();
    while (mFrameThreadRunning) {
        LOGV("initPreview: waiting for old frame thread to complete.");
        mFrameThreadWait.wait(mFrameThreadWaitLock);
        LOGV("initPreview: old frame thread completed.");
    }
    mFrameThreadWaitLock.unlock();

    mSnapshotThreadWaitLock.lock();
    while (mSnapshotThreadRunning) {
        LOGV("initPreview: waiting for old snapshot thread to complete.");
        mSnapshotThreadWait.wait(mSnapshotThreadWaitLock);
        LOGV("initPreview: old snapshot thread completed.");
    }
    mSnapshotThreadWaitLock.unlock();

    mPreviewFrameSize = mPreviewWidth * mPreviewHeight * 3/2;
    // FIX Remove PreviewPmemPool since there is no code there
    mPreviewHeap = new PreviewPmemPool(mCameraControlFd,
                                       mPreviewWidth * mPreviewHeight * 2,
                                       kPreviewBufferCount,
                                       mPreviewFrameSize,
                                       0,
                                       "preview");

    if (!mPreviewHeap->initialized()) {
        mPreviewHeap.clear();
        LOGE("initPreview X: could not initialize preview heap.");
        return false;
    }

    mDimension.picture_width = DEFAULT_PICTURE_WIDTH;
    mDimension.picture_height = DEFAULT_PICTURE_HEIGHT;

    unsigned char activeBuffer;

    // (sizeof(mDimension) == 0x70) found in assembled codes
    // element type was unsigned long?
    if (native_set_dimension(&mDimension)) {
        for (int cnt = 0; cnt < kPreviewBufferCount; cnt++) {
            frames[cnt].fd = mPreviewHeap->mHeap->getHeapID();
            // virtual addrs(buffer) of 4 frames wouldn't be changed in Donut dmesg
            frames[cnt].buffer = (uint32_t)mPreviewHeap->mHeap->base();
            // y_off and cbcr_off confirmed in Donut(just for 240x160)
            // math operations found in assembled codes
            frames[cnt].y_off = cnt * 0x0C800;
            frames[cnt].cbcr_off = cnt * 0x0C800 +
                mDimension.display_width * mDimension.display_height;

            if (frames[cnt].buffer == 0) {
                LOGV("frames[%d].buffer: malloc failed!", cnt);
                return false;
            }

            frames[cnt].path = MSM_FRAME_ENC;

            activeBuffer = (cnt != kPreviewBufferCount - 1) ? 1 : 0;

            // returned type should be bool, verified from assembled codes
            native_register_preview_bufs(mCameraControlFd,
                                         &mDimension,
                                         &frames[cnt],
                                         activeBuffer);

            if (cnt == kPreviewBufferCount - 1) {
                LOGV("set preview callback");
                // firesnatch, this call is not supported
                //LINK_cam_set_frame_callback();

                // TODO: memset(*s, 0, 0x14)

                mFrameThreadRunning = !pthread_create(&mFrameThread,
                                                      NULL,
                                                      cam_frame_click, //frame_thread,
                                                      &frames[cnt]);
                if (mFrameThreadRunning)
                    LOGV("Preview thread created");
                else
                    LOGE("Preview thread error");

                bFramePresent=false;
                mPrevThreadRunning = !pthread_create(&mPrevThread,
                                                      NULL,
                                                      prev_frame_click, //frame_thread,
                                                      NULL);
                if (mPrevThreadRunning)
                    LOGV("Preview NEW thread created");
                else
                    LOGE("Preview NEW thread error");
            }
        }
    } else
        LOGE("native_set_dimension failed");

    return mFrameThreadRunning;
}

void QualcommCameraHardware::deinitPreview(void)
{
    LOGV("deinitPreview E");

    // When we call deinitPreview(), we signal to the frame thread that it
    // needs to exit, but we DO NOT WAIT for it to complete here.  The problem
    // is that deinitPreview is sometimes called from the frame-thread's
    // callback, when the refcount on the Camera client reaches zero.  If we
    // called pthread_join(), we would deadlock.  So, we just call
    // LINK_camframe_terminate() in deinitPreview(), which makes sure that
    // after the preview callback returns, the camframe thread will exit.  We
    // could call pthread_join() in initPreview() to join the last frame
    // thread.  However, we would also have to call pthread_join() in release
    // as well, shortly before we destoy the object; this would cause the same
    // deadlock, since release(), like deinitPreview(), may also be called from
    // the frame-thread's callback.  This we have to make the frame thread
    // detached, and use a separate mechanism to wait for it to complete.

    // camframe_terminate() never been used

    if (mFrameThreadRunning) {
        // Send a exit signal to stop the frame thread
        if (!pthread_kill(mFrameThread, SIGUSR1)) {
            LOGV("terminate frame_thread successfully");
            mFrameThreadRunning = false;
        } else
            LOGE("frame_thread doesn't exist");
    }

    if (mPrevThreadRunning) {
        // Send a exit signal to stop the frame thread
        if (!pthread_kill(mPrevThread, SIGUSR1)) {
            LOGV("terminate preview procesor frame_thread successfully");
            mPrevThreadRunning = false;
        } else
            LOGE("preview procesor frame_thread doesn't exist");
    }

    LOGV("Unregister preview buffers");
    for (int cnt = 0; cnt < kPreviewBufferCount; ++cnt) {
        native_unregister_preview_bufs(mCameraControlFd,
                                       &mDimension,
                                       &frames[cnt]);
    }

    mPreviewHeap.clear();

    LOGV("deinitPreview X");
}

bool QualcommCameraHardware::initRaw(bool initJpegHeap)
{
    LOGV("initRaw E: picture size=%dx%d", mRawWidth, mRawHeight); // 2048x1536

    mDimension.picture_width   = mRawWidth;
    mDimension.picture_height  = mRawHeight;
    mRawSize = mRawWidth * mRawHeight * 3 / 2;
    mJpegMaxSize = mRawWidth * mRawHeight * 3 / 2;

    if(!native_set_dimension(&mDimension)) {
        LOGE("initRaw X: failed to set dimension");
        return false;
    }

    if (mJpegHeap != NULL) {
        LOGV("initRaw: clearing old mJpegHeap.");
        mJpegHeap.clear();
    }

    // Thumbnails

    LOGV("initRaw: initializing mThumbHeap. with size %d", THUMBNAIL_BUFFER_SIZE); // 41472
    mThumbnailHeap =
        new PmemPool("/dev/pmem_adsp",
                     mCameraControlFd,
                     MSM_PMEM_THUMBNAIL,
                     THUMBNAIL_BUFFER_SIZE,
                     1,
                     THUMBNAIL_BUFFER_SIZE,
                     0,
                     "thumbnail camera");

    if (!mThumbnailHeap->initialized()) {
        mThumbnailHeap.clear();
        mRawHeap.clear();
        LOGE("initRaw X failed: error initializing mThumbnailHeap.");
        return false;
    }

    // Snapshot

    LOGV("initRaw: initializing mRawHeap. with size %d", mRawSize); // 4718592
    mRawHeap =
        new PmemPool("/dev/pmem_adsp",
                     mCameraControlFd,
                     MSM_PMEM_MAINIMG,
                     mJpegMaxSize,
                     kRawBufferCount,
                     mRawSize,
                     0,
                     "snapshot camera");

    if (!mRawHeap->initialized()) {
        LOGE("initRaw X failed with pmem_camera, trying with pmem_adsp");
        mRawHeap =
            new PmemPool("/dev/pmem_adsp",
                         mCameraControlFd,
                         MSM_PMEM_MAINIMG,
                         mJpegMaxSize,
                         kRawBufferCount,
                         mRawSize,
                         0,
                         "snapshot camera");
        if (!mRawHeap->initialized()) {
            mRawHeap.clear();
            LOGE("initRaw X: error initializing mRawHeap");
            return false;
        }
    }

    LOGV("do_mmap snapshot pbuf = %p, pmem_fd = %d",
         (uint8_t *)mRawHeap->mHeap->base(), mRawHeap->mHeap->getHeapID());

    // Jpeg

    if (initJpegHeap) {
        LOGV("initRaw: initializing mJpegHeap.");
        mJpegHeap =
            new AshmemPool(mJpegMaxSize,
                           kJpegBufferCount,
                           0, // we do not know how big the picture wil be
                           0,
                           "jpeg");

        if (!mJpegHeap->initialized()) {
            mJpegHeap.clear();
            mRawHeap.clear();
            LOGE("initRaw X failed: error initializing mJpegHeap.");
            return false;
        }
    }

    mRawInitialized = true;

    LOGV("initRaw X success");
    return true;
}

void QualcommCameraHardware::deinitRaw()
{
    LOGV("deinitRaw E");

    mThumbnailHeap.clear();
    mJpegHeap.clear();
    mRawHeap.clear();
    mRawInitialized = false;

    LOGV("deinitRaw X");
}

void QualcommCameraHardware::release()
{
    LOGV("release E");
    Mutex::Autolock l(&mLock);

#if DLOPEN_LIBMMCAMERA
    if (libmmcamera == NULL) {
        LOGE("ERROR: multiple release!");
        return;
    }
#else
#warning "Cannot detect multiple release when not dlopen()ing libmmcamera!"
#endif

    int rc;
    struct msm_ctrl_cmd_t ctrlCmd;

    if (mCameraRunning) {
        if (mMsgEnabled & CAMERA_MSG_VIDEO_FRAME) {
            mRecordFrameLock.lock();
            mReleasedRecordingFrame = true;
            mRecordWait.signal();
            mRecordFrameLock.unlock();
        }
        stopPreviewInternal();
    }

    if (mRawInitialized) deinitRaw();

    LOGV("CAMERA_EXIT");

    ctrlCmd.timeout_ms = 5000;
    ctrlCmd.length = 0;
    ctrlCmd.type = (uint16_t)CAMERA_EXIT;

    if (ioctl(mCameraControlFd, MSM_CAM_IOCTL_CTRL_COMMAND, &ctrlCmd) < 0)
        LOGE("ioctl CAMERA_EXIT fd %d error %s",
             mCameraControlFd, strerror(errno));

    LOGV("Stopping the conf thread");
    rc = pthread_join(mCamConfigThread, NULL);
    if (rc)
        LOGE("config_thread exit failure: %s", strerror(errno));

    if (mJpegThreadRunning) {
        LOGV("Stopping the jpeg thread");
        rc = pthread_join(jpegThread, NULL);
        if (rc)
            LOGE("jpeg_thread exit failure: %s", strerror(errno));
    }

    memset(&mDimension, 0, sizeof(mDimension));

    close(mCameraControlFd);
    mCameraControlFd = -1;

    close(fd_frame);
    fd_frame = -1;

#if DLOPEN_LIBMMCAMERA
    if (libmmcamera) {
        ::dlclose(libmmcamera);
        LOGV("dlclose(libmmcamera)");
        libmmcamera = NULL;
    }
    if (libmmcamera_target) {
        ::dlclose(libmmcamera_target);
        LOGV("dlclose(libmmcamera_target)");
        libmmcamera_target = NULL;
    }
#endif

    // why ~QualcommCameraHardware() not called --> fixed!
    // firesnatch - removed locking
    //Mutex::Autolock lock(&singleton_lock);

    Mutex::Autolock lock(&singleton_lock);
    singleton.clear();
    singleton_releasing = false;
    singleton_wait.signal();

    LOGV("release X");
}

QualcommCameraHardware::~QualcommCameraHardware()
{
    LOGD("~QualcommCameraHardware E");
    // firesnatch - relocated to the end of release() function
    //Mutex::Autolock lock(&singleton_lock);
    //singleton.clear();
    //singleton_releasing = false;
    //singleton_wait.signal();
    LOGD("~QualcommCameraHardware X");
}

sp<IMemoryHeap> QualcommCameraHardware::getRawHeap() const
{
    LOGV("getRawHeap");
    return mRawHeap != NULL ? mRawHeap->mHeap : NULL;
}

sp<IMemoryHeap> QualcommCameraHardware::getPreviewHeap() const
{
    LOGV("getPreviewHeap");
    return mPreviewHeap != NULL ? mPreviewHeap->mHeap : NULL;
}

status_t QualcommCameraHardware::startPreviewInternal()
{
    LOGV("startPreview E");

    if (mCameraRunning) {
        LOGV("startPreview X: preview already running.");
        return NO_ERROR;
    }

    if (!mPreviewInitialized) {
        mPreviewInitialized = initPreview();
        if (!mPreviewInitialized) {
            LOGE("startPreview X initPreview failed. Not starting preview.");
            return UNKNOWN_ERROR;
        }
    }

    mCameraRunning = native_start_preview(mCameraControlFd);
    if (!mCameraRunning) {
        deinitPreview();
        mPreviewInitialized = false;
        LOGE("startPreview X: native_start_preview failed!");
        return UNKNOWN_ERROR;
    }

    LOGV("startPreview X");
    return NO_ERROR;
}

status_t QualcommCameraHardware::startPreview()
{
    Mutex::Autolock l(&mLock);
    return startPreviewInternal();
}

void QualcommCameraHardware::stopPreviewInternal()
{
    LOGV("stopPreviewInternal E with mCameraRunning %d", mCameraRunning);
    if (mCameraRunning) {
        // Cancel auto focus.
        if (mMsgEnabled & CAMERA_MSG_FOCUS) {
            LOGV("canceling autofocus");
            cancelAutoFocus();
        }

        LOGV("Stopping preview");
        mCameraRunning = !native_stop_preview(mCameraControlFd);
        if (!mCameraRunning && mPreviewInitialized) {
            deinitPreview();
            mPreviewInitialized = false;
        }
        else LOGE("stopPreviewInternal: failed to stop preview");
    }
    LOGV("stopPreviewInternal X with mCameraRunning %d", mCameraRunning);
}

void QualcommCameraHardware::stopPreview()
{
    LOGV("stopPreview: E");
    Mutex::Autolock l(&mLock);

    if(mMsgEnabled & CAMERA_MSG_VIDEO_FRAME)
        return;

    if (mCameraRunning)
        stopPreviewInternal();

    LOGV("stopPreview: X");
}

void QualcommCameraHardware::runAutoFocus()
{
    int status = false;

    mAutoFocusThreadLock.lock();
    mAutoFocusFd = open(MSM_CAMERA_CONTROL, O_RDWR);
    if (mAutoFocusFd < 0) {
        LOGE("autofocus: cannot open %s: %s",
             MSM_CAMERA_CONTROL,
             strerror(errno));
        mAutoFocusThreadRunning = false;
        mAutoFocusThreadLock.unlock();
        return;
    }

#if DLOPEN_LIBMMCAMERA
    // We need to maintain a reference to libmmcamera.so for the duration of the
    // AF thread, because we do not know when it will exit relative to the
    // lifetime of this object.  We do not want to dlclose() libmmcamera while
    // LINK_cam_frame is still running.
    void *libhandle = ::dlopen("libmmcamera.so", RTLD_NOW);
    LOGV("AF: loading libmmcamera at %p", libhandle);
    if (!libhandle) {
        LOGE("FATAL ERROR: could not dlopen libmmcamera.so: %s", dlerror());
        close(mAutoFocusFd);
        mAutoFocusFd = -1;
        mAutoFocusThreadRunning = false;
        mAutoFocusThreadLock.unlock();
        return;
    }
#endif

    /* This will block until either AF completes or is cancelled. */
    native_set_afmode(camerafd, AF_MODE_MACRO);
    if (native_get_af_result(camerafd) == 0) {
        status = true;
    }

    mAutoFocusThreadRunning = false;
    close(mAutoFocusFd);
    mAutoFocusFd = -1;
    mAutoFocusThreadLock.unlock();
    if (mMsgEnabled & CAMERA_MSG_FOCUS) {
        mNotifyCb(CAMERA_MSG_FOCUS, status, 0, mCallbackCookie);
    }

#if DLOPEN_LIBMMCAMERA
    if (libhandle) {
        ::dlclose(libhandle);
        LOGV("AF: dlclose(libmmcamera)");
    }
#endif
}

status_t QualcommCameraHardware::cancelAutoFocus()
{
    // firesnatch - removed call
    //native_cancel_afmode(mCameraControlFd, mAutoFocusFd);
    /* Needed for eclair camera PAI */
    return NO_ERROR;
}


void QualcommCameraHardware::setZoom()
{
    int32_t level;
    int32_t multiplier;
    if(native_get_maxzoom(mCameraControlFd,
            (void *)&mMaxZoom) == true){
        LOGD("Maximum zoom value is %d", mMaxZoom);
        //maxZoom/5 in the ideal world, but it's stuck on 90
        multiplier = getParm("picture-size", picturesize);

        //Camcorder mode uses preview size
        if (strcmp(mParameters.get("preview-frame-rate"),"15") != 0)
            multiplier = getParm("preview-size", picturesize);

        zoomSupported = true;
        if(mMaxZoom > 0){
            level = mParameters.getInt(CameraParameters::KEY_ZOOM) * multiplier;
        }
    } else {
        zoomSupported = false;
        LOGE("Failed to get maximum zoom value...setting max "
                "zoom to zero");
        mMaxZoom = 0;
    }

    if (level >= mMaxZoom) {
        level = mMaxZoom;
    }

    LOGV("Set Zoom level: %d current: %d maximum: %d", level, mCurZoom, mMaxZoom);

    if (level == mCurZoom) {
        return;
    }

   if (level != -1) {
            LOGV("Final Zoom Level: %d", level);
            if (level >= 0 && level <= mMaxZoom) {
                native_set_parm(CAMERA_SET_PARM_ZOOM, sizeof(level), (void *)&level);
                mCurZoom = level;
            }
    }
}

void *auto_focus_thread(void *user)
{
    sp<QualcommCameraHardware> obj = QualcommCameraHardware::getInstance();
    if (obj != 0) {
        obj->runAutoFocus();
    }
    else LOGW("not starting autofocus: the object went away!");

    return NULL;
}

status_t QualcommCameraHardware::autoFocus()
{
    Mutex::Autolock l(&mLock);

    if (mCameraControlFd < 0) {
        LOGE("not starting autofocus: main control fd %d", mCameraControlFd);
        return UNKNOWN_ERROR;
    }

    {
        mAutoFocusThreadLock.lock();
        if (!mAutoFocusThreadRunning) {
            // Create a detatched thread here so that we don't have to wait
            // for it when we cancel AF.
            pthread_t thr;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            mAutoFocusThreadRunning =
                !pthread_create(&thr, &attr,
                                auto_focus_thread, NULL);
            if (!mAutoFocusThreadRunning) {
                LOGE("failed to start autofocus thread");
                mAutoFocusThreadLock.unlock();
                return UNKNOWN_ERROR;
            }
        }
        mAutoFocusThreadLock.unlock();
    }

    return NO_ERROR;
}

void QualcommCameraHardware::runSnapshotThread(void *data)
{
    LOGV("runSnapshotThread E");

    if (native_start_snapshot(mCameraControlFd))
        receiveRawPicture();
    else
        LOGE("main: native_start_snapshot failed!");

    mSnapshotThreadWaitLock.lock();
    mSnapshotThreadRunning = false;
    mSnapshotThreadWait.signal();
    mSnapshotThreadWaitLock.unlock();

    LOGV("runSnapshotThread X");
}

void *snapshot_thread(void *user)
{
    LOGV("snapshot_thread E");
    sp<QualcommCameraHardware> obj = QualcommCameraHardware::getInstance();
    if (obj != 0) {
        obj->runSnapshotThread(user);
    }
    else LOGW("not starting snapshot thread: the object went away!");
    LOGV("snapshot_thread X");
    return NULL;
}

status_t QualcommCameraHardware::takePicture()
{
    LOGV("takePicture: E");
    Mutex::Autolock l(&mLock);

    // Wait for old snapshot thread to complete.
    mSnapshotThreadWaitLock.lock();
    while (mSnapshotThreadRunning) {
        LOGV("takePicture: waiting for old snapshot thread to complete.");
        mSnapshotThreadWait.wait(mSnapshotThreadWaitLock);
        LOGV("takePicture: old snapshot thread completed.");
    }

    if (mCameraRunning)
        stopPreviewInternal();

    if (!initRaw(mMsgEnabled & CAMERA_MSG_COMPRESSED_IMAGE)) {
        LOGE("initRaw failed. Not taking picture.");
        return UNKNOWN_ERROR;
    }

    mShutterLock.lock();
    mShutterPending = true;
    mShutterLock.unlock();

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    mSnapshotThreadRunning = !pthread_create(&mSnapshotThread,
                                             &attr,
                                             snapshot_thread,
                                             NULL);
    mSnapshotThreadWaitLock.unlock();

    setZoom();

    LOGV("takePicture: X");
    return mSnapshotThreadRunning ? NO_ERROR : UNKNOWN_ERROR;
}

status_t QualcommCameraHardware::cancelPicture()
{
    LOGV("cancelPicture: EX");
    return NO_ERROR;
}

//#if 0
void QualcommCameraHardware::initCameraParameters()
{
    LOGV("initCameraParameters: E");

#define SET_PARM(x, y) do {                                        \
    LOGV("initCameraParameters: set parm: %s, %d", #x, y);         \
    native_set_parm(x, sizeof(y), (void *)&(y));                   \
} while(0)

    int32_t value;
    value = getParm("effect", effect);
    SET_PARM(CAMERA_SET_PARM_EFFECT, value);

    value = getParm("whitebalance", whitebalance);
    SET_PARM(CAMERA_SET_PARM_WB, value);

    value = getParm("antibanding", antibanding);
    SET_PARM(CAMERA_SET_PARM_ANTIBANDING, value);

    value = getParm("flash-mode", flashmode);
    SET_PARM(CAMERA_SET_PARM_LED_MODE, value);

    setZoom();

#undef SET_PARM

    LOGV("initCameraParameters: X");
}
//#endif

status_t QualcommCameraHardware::setParameters(
        const CameraParameters& params)
{
    LOGV("setParameters: E params = %p", &params);

    Mutex::Autolock l(&mLock);

    // Set preview size.
    preview_size_type *ps = preview_sizes;
    {
        int width, height;

        params.getPreviewSize(&width, &height);
        LOGV("requested size %d x %d", width, height);
        // Validate the preview sizes
        size_t i;
        for (i = 0; i < PREVIEW_SIZE_COUNT; ++i, ++ps) {
            if (width == ps->width && height == ps->height)
                break;
        }
        if (i == PREVIEW_SIZE_COUNT) {
            LOGE("Invalid preview size requested: %dx%d",
                 width, height);
            return BAD_VALUE;
        }
    }

    mPreviewWidth = mDimension.display_width = ps->width;
    mPreviewHeight = mDimension.display_height = ps->height;

    params.getPictureSize(&mRawWidth, &mRawHeight);
    mDimension.picture_width = mRawWidth;
    mDimension.picture_height = mRawHeight;

    // Set up the jpeg-thumbnail-size parameters.
    {
        int val;

        val = params.getInt("jpeg-thumbnail-width");
        if (val < 0) {
            mDimension.ui_thumbnail_width= THUMBNAIL_WIDTH;
            LOGW("jpeg-thumbnail-width is not specified: defaulting to %d",
                 THUMBNAIL_WIDTH);
        }
        else mDimension.ui_thumbnail_width = val;

        val = params.getInt("jpeg-thumbnail-height");
        if (val < 0) {
            mDimension.ui_thumbnail_height= THUMBNAIL_HEIGHT;
            LOGW("jpeg-thumbnail-height is not specified: defaulting to %d",
                 THUMBNAIL_HEIGHT);
        }
        else mDimension.ui_thumbnail_height = val;
    }

    // setParameters
    mParameters = params;

    // Effect, WhiteBalance, AntiBanding...
    if (mCameraControlFd != -1)
        initCameraParameters();

    LOGV("setParameters: X");
    return NO_ERROR;
}

CameraParameters QualcommCameraHardware::getParameters() const
{
    LOGV("getParameters: EX");
    return mParameters;
}

extern "C" sp<CameraHardwareInterface> openCameraHardware()
{
    return QualcommCameraHardware::createInstance();
}

wp<QualcommCameraHardware> QualcommCameraHardware::singleton;

// If the hardware already exists, return a strong pointer to the current
// object. If not, create a new hardware object, put it in the singleton,
// and return it.
sp<CameraHardwareInterface> QualcommCameraHardware::createInstance()
{
    LOGD("Revision: %s%s", REVISION_C, REVISION_H);

    LOGV("createInstance: E");

    // firesnatch - removed this locking code
    ///LOGV("get into singleton lock");
    //Mutex::Autolock lock(&singleton_lock);
    // Wait until the previous release is done.
    //while (singleton_releasing) {
    //    LOGD("Wait for previous release.");
    //    singleton_wait.wait(singleton_lock);
    //}

    if (singleton != 0) {
        sp<CameraHardwareInterface> hardware = singleton.promote();
        if (hardware != 0) {
            LOGD("createInstance: X return existing hardware=%p", &(*hardware));
            return hardware;
        }
    }

    {
        struct stat st;
        int rc = stat("/dev/oncrpc", &st);
        if (rc < 0) {
            LOGD("createInstance: X failed to create hardware: %s", strerror(errno));
            return NULL;
        }
    }

    QualcommCameraHardware *cam = new QualcommCameraHardware();
    sp<QualcommCameraHardware> hardware(cam);
    singleton = hardware;

    cam->initDefaultParameters();
    cam->startCamera();

    LOGV("createInstance: X created hardware=%p", &(*hardware));
    return hardware;
}

// For internal use only, hence the strong pointer to the derived type.
sp<QualcommCameraHardware> QualcommCameraHardware::getInstance()
{
    sp<CameraHardwareInterface> hardware = singleton.promote();
    if (hardware != 0) {
        return sp<QualcommCameraHardware>(static_cast<QualcommCameraHardware*>(hardware.get()));
    } else {
        LOGV("getInstance: X new instance of hardware");
        return sp<QualcommCameraHardware>();
    }
}

// passes the Addresses to CameraService to getPreviewHeap
void QualcommCameraHardware::receivePreviewFrame(struct msm_frame_t *frame)
{
    if ( LOG_PREVIEW )
        LOGV("receivePreviewFrame E");

    if (!mCameraRunning) {
        LOGE("ignoring preview callback--camera has been stopped");
        return;
    }

    // Find the offset within the heap of the current buffer.
    ssize_t offset =
        (ssize_t)frame->buffer - (ssize_t)mPreviewHeap->mHeap->base();
    offset /= mPreviewFrameSize;

    mInPreviewCallback = true;
    if (mMsgEnabled & CAMERA_MSG_PREVIEW_FRAME)
        mDataCb(CAMERA_MSG_PREVIEW_FRAME, mPreviewHeap->mBuffers[offset], mCallbackCookie);

    if (mMsgEnabled & CAMERA_MSG_VIDEO_FRAME) {
        Mutex::Autolock rLock(&mRecordFrameLock);
        mDataCbTimestamp(systemTime(), CAMERA_MSG_VIDEO_FRAME,
            mPreviewHeap->mBuffers[offset], mCallbackCookie);

        if (mReleasedRecordingFrame != true) {
            LOGV("block for release frame request/command");
            // firesnatch - removed, not supported
            //if (!LINK_cam_release_frame())
            //    LOGE("cam_release_frame failed");
            mRecordWait.wait(mRecordFrameLock);
        }
        mReleasedRecordingFrame = false;
    }

    mInPreviewCallback = false;

    if ( LOG_PREVIEW )
        LOGV("receivePreviewFrame X");
}

status_t QualcommCameraHardware::startRecording()
{
    LOGV("startRecording E");
    updateVideoLightMode(1);
    Mutex::Autolock l(&mLock);

    mReleasedRecordingFrame = false;
    mCameraRecording = true;

    return startPreviewInternal();
}

void QualcommCameraHardware::stopRecording()
{
    LOGV("stopRecording: E");
    updateVideoLightMode(0);
    Mutex::Autolock l(&mLock);

    {
        mRecordFrameLock.lock();
        mReleasedRecordingFrame = true;
        mRecordWait.signal();
        mRecordFrameLock.unlock();

        mCameraRecording = false;

        if(mMsgEnabled & CAMERA_MSG_PREVIEW_FRAME) {
            LOGV("stopRecording: X, preview still in progress");
            return;
        }
    }

    if (mCameraRunning)
        stopPreviewInternal();
    LOGV("stopRecording: X");
}

void QualcommCameraHardware::releaseRecordingFrame(
       const sp<IMemory>& mem __attribute__((unused)))
{
    LOGV("releaseRecordingFrame E");
    Mutex::Autolock l(&mLock);
    Mutex::Autolock rLock(&mRecordFrameLock);
    // firesnatch - removed, not supported
    //if (!LINK_cam_release_frame())
    //    LOGE("cam_release_frame failed");
    mReleasedRecordingFrame = true;
    mRecordWait.signal();
    LOGV("releaseRecordingFrame X");
}

bool QualcommCameraHardware::recordingEnabled()
{
    LOGV("recordingEnabled");
    return (mCameraRunning && mCameraRecording);
}

void QualcommCameraHardware::notifyShutter()
{
    mShutterLock.lock();
    if (mShutterPending && (mMsgEnabled & CAMERA_MSG_SHUTTER)) {
        mNotifyCb(CAMERA_MSG_SHUTTER, 0, 0, mCallbackCookie);
        mShutterPending = false;
    }
    mShutterLock.unlock();
}

void QualcommCameraHardware::receiveRawPicture()
{
    LOGV("receiveRawPicture: E");

    notifyShutter();

    if (mMsgEnabled & CAMERA_MSG_RAW_IMAGE) {
        LOGV("before native_get_picture");
        if(native_get_picture(mCameraControlFd, &mCrop) == false) {
            LOGE("getPicture failed!");
            return;
        }
        mDataCb(CAMERA_MSG_RAW_IMAGE, mRawHeap->mBuffers[0], mCallbackCookie);
    }
    else LOGV("Raw-picture callback was canceled--skipping.");

    if (mMsgEnabled & CAMERA_MSG_COMPRESSED_IMAGE) {
        mJpegSize = mRawWidth * mRawHeight * 3/2;
        LOGV("Before JPEG Encoder Init");
        if (LINK_jpeg_encoder_init()) {
            if(native_jpeg_encode()) {
                LOGV("receiveRawPicture: X (success)");
                return;
            }
            LOGE("jpeg encoding failed");
        }
        else LOGE("receiveRawPicture X: jpeg_encoder_init failed.");
    }
    else LOGV("JPEG callback is NULL, not encoding image.");

    if (mRawInitialized)
        deinitRaw();

    LOGV("receiveRawPicture: X");
}

void QualcommCameraHardware::receiveJpegPictureFragment(
    uint8_t *buff_ptr, uint32_t buff_size)
{
    uint32_t remaining = mJpegHeap->mHeap->virtualSize();
    remaining -= mJpegSize;
    uint8_t *base = (uint8_t *)mJpegHeap->mHeap->base();

    LOGV("receiveJpegPictureFragment size %d", buff_size);
    if (buff_size > remaining) {
        LOGE("receiveJpegPictureFragment: size %d exceeds what "
             "remains in JPEG heap (%d), truncating",
             buff_size,
             remaining);
        buff_size = remaining;
    }
    memcpy(base + mJpegSize, buff_ptr, buff_size);
    mJpegSize += buff_size;
}

void QualcommCameraHardware::receiveJpegPicture(void)
{
    LOGV("receiveJpegPicture: E image (%d uint8_ts out of %d)",
         mJpegSize, mJpegHeap->mBufferSize);

        LOGD("mJpegHeap->mFrameOffset %d", mJpegHeap->mFrameOffset ) ;

    int index = 0, rc;

    if (mMsgEnabled & CAMERA_MSG_COMPRESSED_IMAGE) {
        // The reason we do not allocate into mJpegHeap->mBuffers[offset] is
        // that the JPEG image's size will probably change from one snapshot
        // to the next, so we cannot reuse the MemoryBase object.
        sp<MemoryBase> buffer = new
            MemoryBase(mJpegHeap->mHeap,
                       index * mJpegHeap->mBufferSize +
                       mJpegHeap->mFrameOffset,
                       mJpegSize);

        mDataCb(CAMERA_MSG_COMPRESSED_IMAGE, buffer, mCallbackCookie);
        buffer = NULL;
    }
    else LOGV("JPEG callback was cancelled--not delivering image.");

    if (mRawInitialized)
        deinitRaw();

    LOGV("receiveJpegPicture: X callback done.");
}

bool QualcommCameraHardware::previewEnabled()
{
    Mutex::Autolock l(&mLock);
    return (mCameraRunning && (mMsgEnabled & CAMERA_MSG_PREVIEW_FRAME));
}

int QualcommCameraHardware::getParm(
    const char *parm_str, const struct str_map *const parm_map)
{
    // Check if the parameter exists.
    const char *str = mParameters.get(parm_str);
    if (str == NULL) return NOT_FOUND;

    // Look up the parameter value.
    return attr_lookup(parm_map, str);
}

void QualcommCameraHardware::setEffect()
{
    int32_t value = getParm("effect", effect);
    if (value != NOT_FOUND) {
        native_set_parm(CAMERA_SET_PARM_EFFECT, sizeof(value), (void *)&value);
    }
}

void QualcommCameraHardware::setWhiteBalance()
{
    int32_t value = getParm("whitebalance", whitebalance);
    if (value != NOT_FOUND) {
        native_set_parm(CAMERA_SET_PARM_WB, sizeof(value), (void *)&value);
    }
}

void QualcommCameraHardware::setAntibanding()
{
    camera_antibanding_type value =
        (camera_antibanding_type) getParm("antibanding", antibanding);
    native_set_parm(CAMERA_SET_PARM_ANTIBANDING, sizeof(value), (void *)&value);
}

void QualcommCameraHardware::setFlashMode()
{
    led_mode_t value =
        (led_mode_t) getParm("flash-mode", flashmode);
    native_set_parm(CAMERA_SET_PARM_LED_MODE, sizeof(value), (void *)&value);
}

void QualcommCameraHardware::updateVideoLightMode(int starting)
{
    led_mode_t value = (starting == 1 ? (led_mode_t) getParm("flash-mode", flashmode) : LED_MODE_OFF); //moto-specific flash

#ifdef USE_BUGGY_QUALCOMM_ISP_FOR_VIDEOLIGHT
    native_set_parm(CAMERA_SET_PARM_LED_MODE, sizeof(value), (void *)&value);
#else
    {
    FILE *fd = fopen("/sys/class/leds/cam-torch/brightness", "w");
    if (fd) {
         if (fputs(value != LED_MODE_OFF ? "255" : "0", fd) < 0) {
             LOGE("updateVideoLightMode: virtual file write failed !\n");
         } else {
             LOGV("updateVideoLightMode '%s' successful", value != LED_MODE_OFF ? "On" : "Off");
         }
         fclose(fd);
     } else {
         LOGE("updateVideoLightMode: virtual file fopen() failed !\n");
     }
     }
#endif
}

QualcommCameraHardware::MemPool::MemPool(int buffer_size, int num_buffers,
                                         int frame_size,
                                         int frame_offset,
                                         const char *name) :
    mBufferSize(buffer_size),
    mNumBuffers(num_buffers),
    mFrameSize(frame_size),
    mFrameOffset(frame_offset),
    mBuffers(NULL), mName(name)
{
    // empty
}

void QualcommCameraHardware::MemPool::completeInitialization()
{
    // If we do not know how big the frame will be, we wait to allocate
    // the buffers describing the individual frames until we do know their
    // size.

    if (mFrameSize > 0) {
        mBuffers = new sp<MemoryBase>[mNumBuffers];
        for (int i = 0; i < mNumBuffers; i++) {
            mBuffers[i] = new
                MemoryBase(mHeap,
                           i * mBufferSize + mFrameOffset,
                           mFrameSize);
        }
    }
}

QualcommCameraHardware::AshmemPool::AshmemPool(int buffer_size, int num_buffers,
                                               int frame_size,
                                               int frame_offset,
                                               const char *name) :
    QualcommCameraHardware::MemPool(buffer_size,
                                    num_buffers,
                                    frame_size,
                                    frame_offset,
                                    name)
{
    LOGV("constructing MemPool %s backed by ashmem: "
         "%d frames @ %d uint8_ts, offset %d, "
         "buffer size %d",
         mName,
         num_buffers, frame_size, frame_offset, buffer_size);

    int page_mask = getpagesize() - 1;
    int ashmem_size = buffer_size * num_buffers;
    ashmem_size += page_mask;
    ashmem_size &= ~page_mask;

    mHeap = new MemoryHeapBase(ashmem_size);

    completeInitialization();
}

static bool register_buf(int camfd,
                         int size,
                         int pmempreviewfd,
                         uint32_t offset,
                         uint8_t *buf,
                         msm_pmem_t pmem_type,
                         bool active,
                         bool register_buffer = true);

QualcommCameraHardware::PmemPool::PmemPool(const char *pmem_pool,
                                           int camera_control_fd,
                                           msm_pmem_t pmem_type,
                                           int buffer_size,
                                           int num_buffers,
                                           int frame_size,
                                           int frame_offset,
                                           const char *name) :
    QualcommCameraHardware::MemPool(buffer_size,
                                    num_buffers,
                                    frame_size,
                                    frame_offset,
                                    name),
    mPmemType(pmem_type),
    mCameraControlFd(camera_control_fd)
{
    LOGV("constructing MemPool %s backed by pmem pool %s: "
         "%d frames @ %d bytes, offset %d, buffer size %d",
         mName,
         pmem_pool, num_buffers, frame_size, frame_offset,
         buffer_size);

    // Make a new mmap'ed heap that can be shared across processes.

    mAlignedSize = clp2(buffer_size * num_buffers);

    sp<MemoryHeapBase> masterHeap =
        new MemoryHeapBase(pmem_pool, mAlignedSize, 0);
    sp<MemoryHeapPmem> pmemHeap = new MemoryHeapPmem(masterHeap, 0);
    if (pmemHeap->getHeapID() >= 0) {
        pmemHeap->slap();
        masterHeap.clear();
        mHeap = pmemHeap;
        pmemHeap.clear();

        mFd = mHeap->getHeapID();
        if (::ioctl(mFd, PMEM_GET_SIZE, &mSize)) {
            LOGE("pmem pool %s ioctl(PMEM_GET_SIZE) error %s (%d)",
                 pmem_pool, ::strerror(errno), errno);
            mHeap.clear();
            return;
        }

        LOGV("pmem pool %s ioctl(PMEM_GET_SIZE) is %ld", pmem_pool, mSize.len);

        // Register buffers with the camera drivers.
        if (mPmemType != MSM_PMEM_OUTPUT2) {
            for (int cnt = 0; cnt < num_buffers; ++cnt) {
                register_buf(mCameraControlFd,
                             buffer_size,
                             mHeap->getHeapID(),
                             0,
                             (uint8_t *)mHeap->base() + buffer_size * cnt,
                             pmem_type,
                             true);
            }
        }
    }
    else LOGE("pmem pool %s error: could not create master heap!",
              pmem_pool);

    completeInitialization();
}

QualcommCameraHardware::PmemPool::~PmemPool()
{
    LOGV("%s: %s E", __FUNCTION__, mName);

    // Unregister buffers with the camera drivers.
    if (mPmemType != MSM_PMEM_OUTPUT2) {
        for (int cnt = 0; cnt < mNumBuffers; ++cnt) {
            register_buf(mCameraControlFd,
                         mBufferSize,
                         mHeap->getHeapID(),
                         0,
                         (uint8_t *)mHeap->base() + mBufferSize * cnt,
                         mPmemType,
                         true,
                         false /* Unregister */);
        }
    }

    LOGV("destroying PmemPool %s: ", mName);
    LOGV("%s: %s X", __FUNCTION__, mName);
}

QualcommCameraHardware::MemPool::~MemPool()
{
    LOGV("destroying MemPool %s", mName);
    if (mFrameSize > 0)
        delete [] mBuffers;
    mHeap.clear();
    LOGV("destroying MemPool %s completed", mName);
}

QualcommCameraHardware::PreviewPmemPool::PreviewPmemPool(
                        int control_fd,
                        int buffer_size,
                        int num_buffers,
                        int frame_size,
                        int frame_offset,
                        const char *name) :
    QualcommCameraHardware::PmemPool("/dev/pmem_adsp", control_fd, MSM_PMEM_OUTPUT2,
                                 buffer_size,
                                 num_buffers,
                                 frame_size,
                                 frame_offset,
                                 name)
{
    LOGV("QualcommCameraHardware::PreviewPmemPool::PreviewPmemPool");
    if (initialized()) {
        //NOTE : SOME PREVIEWPMEMPOOL SPECIFIC CODE MAY BE ADDED
    }
}

QualcommCameraHardware::PreviewPmemPool::~PreviewPmemPool()
{
    LOGV("destroying PreviewPmemPool");
    if (initialized()) {
        LOGV("releasing PreviewPmemPool memory.");
    }
}

static bool register_buf(int camfd,
                         int size,
                         int pmempreviewfd,
                         uint32_t offset,
                         uint8_t *buf,
                         msm_pmem_t pmem_type,
                         bool active,
                         bool register_buffer)
{
    struct msm_pmem_info_t pmemBuf;

    pmemBuf.type     = pmem_type;
    pmemBuf.fd       = pmempreviewfd;
    pmemBuf.vaddr    = buf;
    pmemBuf.y_off    = 0;
    pmemBuf.active   = active;

    if (pmem_type == MSM_PMEM_RAW_MAINIMG)
        pmemBuf.cbcr_off = 0;
    else
        pmemBuf.cbcr_off = ((size * 2 / 3) + 1) & ~1;

    LOGV("register_buf: camfd = %d, reg = %d buffer = %p",
         camfd, register_buffer, buf);
    if (ioctl(camfd,
              register_buffer ?
              MSM_CAM_IOCTL_REGISTER_PMEM :
              MSM_CAM_IOCTL_UNREGISTER_PMEM,
              &pmemBuf) < 0) {
        LOGE("register_buf: MSM_CAM_IOCTL_(UN)REGISTER_PMEM fd %d error %s",
             camfd,
             strerror(errno));
        return false;
    }
    return true;
}

status_t QualcommCameraHardware::MemPool::dump(int fd, const Vector<String16>& args) const
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    snprintf(buffer, 255, "QualcommCameraHardware::AshmemPool::dump\n");
    result.append(buffer);
    if (mName) {
        snprintf(buffer, 255, "mem pool name (%s)\n", mName);
        result.append(buffer);
    }
    if (mHeap != 0) {
        snprintf(buffer, 255, "heap base(%p), size(%d), flags(%d), device(%s)\n",
                 mHeap->getBase(), mHeap->getSize(),
                 mHeap->getFlags(), mHeap->getDevice());
        result.append(buffer);
    }
    snprintf(buffer, 255, "buffer size (%d), number of buffers (%d),"
             " frame size(%d), and frame offset(%d)\n",
             mBufferSize, mNumBuffers, mFrameSize, mFrameOffset);
    result.append(buffer);
    write(fd, result.string(), result.size());
    return NO_ERROR;
}

static void receive_camframe_callback(struct msm_frame_t *frame)
{
    if ( LOG_PREVIEW )
        LOGV("receive_camframe_callback E");
    sp<QualcommCameraHardware> obj = QualcommCameraHardware::getInstance();
    if (obj != 0) {
        obj->receivePreviewFrame(frame);
    }
    if ( LOG_PREVIEW )
        LOGV("receive_camframe_callback X");
}

static void receive_jpeg_fragment_callback(uint8_t *buff_ptr, uint32_t buff_size)
{
    LOGV("receive_jpeg_fragment_callback E");
    sp<QualcommCameraHardware> obj = QualcommCameraHardware::getInstance();
    if (obj != 0) {
        obj->receiveJpegPictureFragment(buff_ptr, buff_size);
    }
    LOGV("receive_jpeg_fragment_callback X");
}

static void receive_jpeg_callback(jpeg_event_t status)
{
    LOGV("receive_jpeg_callback E (completion status %d)", status);
    if (status == JPEG_EVENT_DONE) {
        sp<QualcommCameraHardware> obj = QualcommCameraHardware::getInstance();
        if (obj != 0) {
            obj->receiveJpegPicture();
        }
    }
    LOGV("receive_jpeg_callback X");
}

status_t QualcommCameraHardware::sendCommand(int32_t command, int32_t arg1,
                                             int32_t arg2)
{
    LOGV("sendCommand: EX");
    return BAD_VALUE;
}

}; // namespace android
