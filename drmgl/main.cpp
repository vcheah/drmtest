#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>

#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <assert.h>

// TEST_EGL_ANDROID_native_fence_sync
PFNEGLCREATESYNCKHRPROC eglCreateSyncKHR = NULL;
PFNEGLDESTROYSYNCKHRPROC eglDestroySyncKHR = NULL;
PFNEGLDUPNATIVEFENCEFDANDROIDPROC eglDupNativeFenceFDANDROID = NULL;
// END TEST_EGL_ANDROID_native_fence_sync

static struct {
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
    EGLSurface surface;	
} gl;

static struct {
    struct gbm_device *dev;
    struct gbm_surface *surface;
} gbm;

static struct {
    int fd;
    drmModeModeInfo *mode;
    uint32_t crtc_id;
    uint32_t connector_id;
} drm;

enum drm_fb_state {
    UNINITIALIZED=0,
    READY_FOR_RENDER=1,
    QUEUED_FOR_FLIP=2,
    FLIPPED_AND_VISIBLE=3
};

struct drm_fb {
    struct gbm_bo *bo;
    uint32_t fb_id;
    drm_fb_state state;
    int drawBufferIndex;
    class PageFlipManager *manager;
};

static uint32_t find_crtc_for_encoder(const drmModeRes *resources,
                                      const drmModeEncoder *encoder) {
    int i;

    for (i = 0; i < resources->count_crtcs; i++) {
        /* possible_crtcs is a bitmask as described here:
         * https://dvdhrm.wordpress.com/2012/09/13/linux-drm-mode-setting-api
         */
        const uint32_t crtc_mask = 1 << i;
        const uint32_t crtc_id = resources->crtcs[i];
        if (encoder->possible_crtcs & crtc_mask) {
            return crtc_id;
        }
    }

    /* no match found */
    return -1;
}

static uint32_t find_crtc_for_connector(const drmModeRes *resources,
                    const drmModeConnector *connector) {
    int i;

    for (i = 0; i < connector->count_encoders; i++) {
        const uint32_t encoder_id = connector->encoders[i];
        drmModeEncoder *encoder = drmModeGetEncoder(drm.fd, encoder_id);

        if (encoder) {
            const uint32_t crtc_id = find_crtc_for_encoder(resources, encoder);

            drmModeFreeEncoder(encoder);
            if (crtc_id != 0) {
                return crtc_id;
            }
        }
    }

    /* no match found */
    return -1;
}

static int init_drm(int desiredWidth, int desiredHeight, int desiredFPS)
{
    drmModeRes *resources;
    drmModeConnector *connector = NULL;
    drmModeEncoder *encoder = NULL;
    int i, area;

    const int max_cards = 8;
    drm.fd = -1;
    for (int card = 0; card < max_cards; card++) {
        char device[20];
        snprintf(device, sizeof(device), "/dev/dri/card%d", card);
        int fd = open(device, O_RDWR);
        if (fd < 0) {
            continue;
        }

        drmModeRes *res = drmModeGetResources(fd);
        if (!res) {
            close(fd);
            continue;
        }

        // Check for connected connectors
        drmModeConnector *conn = NULL;
        for (i = 0; i < res->count_connectors; i++) {
            conn = drmModeGetConnector(fd, res->connectors[i]);
            if (conn->connection == DRM_MODE_CONNECTED) {
                drm.fd = fd;
                resources = res;
                connector = conn;
                goto drm_initialized;
            }
            drmModeFreeConnector(conn);
            conn = NULL;
        }

        drmModeFreeResources(res);
        close(fd);
    }

drm_initialized:

    if (drm.fd < 0 || !connector) {
        /* we could be fancy and listen for hotplug events and wait for
         * a connector..
         */
        printf("no connected connector!\n");
        return -1;
    }

    for (i = 0, area = 0; i < connector->count_modes; i++) {
        drmModeModeInfo *m = &connector->modes[i];
        //pick a mode
        if (m->hdisplay==desiredWidth && m->vdisplay==desiredHeight && m->vrefresh==desiredFPS)
        {
            drm.mode = m;
            break;
        }
        /*
        if (m->type & DRM_MODE_TYPE_PREFERRED) {
            drm.mode = m;
        }

        int current_area = m->hdisplay * m->vdisplay;
        if (current_area > area) {
            drm.mode = m;
            area = current_area;
        }
        */
    }

    if (!drm.mode) {
        printf("could not find mode!\n");
        return -1;
    }

    /* find encoder: */
    for (i = 0; i < resources->count_encoders; i++) {
        encoder = drmModeGetEncoder(drm.fd, resources->encoders[i]);
        if (encoder->encoder_id == connector->encoder_id)
            break;
        drmModeFreeEncoder(encoder);
        encoder = NULL;
    }

    if (encoder) {
        drm.crtc_id = encoder->crtc_id;
    } else {
        uint32_t crtc_id = find_crtc_for_connector(resources, connector);
        if (crtc_id == 0) {
            printf("no crtc found!\n");
            return -1;
        }

        drm.crtc_id = crtc_id;
    }

    drm.connector_id = connector->connector_id;

    return 0;
}

#include <sys/ioctl.h>
#include <libdrm/drm_fourcc.h>

static int init_gbm(uint32_t formatFourCC)
{
    gbm.dev = gbm_create_device(drm.fd);
#if 0
    gbm.surface = gbm_surface_create(gbm.dev,
            drm.mode->hdisplay, drm.mode->vdisplay,
            formatFourCC,
            GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
#else
    uint64_t modifiers[] = { I915_FORMAT_MOD_4_TILED };
    gbm.surface = gbm_surface_create_with_modifiers(gbm.dev, drm.mode->hdisplay, drm.mode->vdisplay, formatFourCC, modifiers, 1);
#endif

    if (!gbm.surface) {
        printf("failed to create gbm surface\n");
        return -1;
    }

    return 0;
}

EGLConfig FindEGLConfig(EGLDisplay display, int bitsNeeded) {
  EGLint numConfigs;
  if (!eglGetConfigs(display, NULL, 0, &numConfigs)) {
    printf("Failed to get number of EGL configurations: %d\n", eglGetError());
    return NULL;
  }

  EGLConfig* configs = (EGLConfig*)malloc(sizeof(EGLConfig) * numConfigs);
  if (!eglGetConfigs(display, configs, numConfigs, &numConfigs)) {
    printf("Failed to get EGL configurations: %d\n", eglGetError());
    free(configs);
    return NULL;
  }

  EGLConfig returnConfig = NULL;

  for (int i = 0; i < numConfigs; i++) {
    EGLint redSize, greenSize, blueSize, alphaSize, depthSize, stencilSize, bufferSize, sampleBuffers, samples;
    EGLint surfaceType;
    eglGetConfigAttrib(display, configs[i], EGL_RED_SIZE, &redSize);
    eglGetConfigAttrib(display, configs[i], EGL_GREEN_SIZE, &greenSize);
    eglGetConfigAttrib(display, configs[i], EGL_BLUE_SIZE, &blueSize);
    eglGetConfigAttrib(display, configs[i], EGL_ALPHA_SIZE, &alphaSize);
    eglGetConfigAttrib(display, configs[i], EGL_DEPTH_SIZE, &depthSize);
    eglGetConfigAttrib(display, configs[i], EGL_STENCIL_SIZE, &stencilSize);
    eglGetConfigAttrib(display, configs[i], EGL_BUFFER_SIZE, &bufferSize);
    eglGetConfigAttrib(display, configs[i], EGL_SAMPLE_BUFFERS, &sampleBuffers);
    eglGetConfigAttrib(display, configs[i], EGL_SAMPLES, &samples);
    eglGetConfigAttrib(display, configs[i], EGL_SURFACE_TYPE, &surfaceType);
    if (sampleBuffers!=0 || stencilSize!=0 || depthSize!=16 || (bufferSize!=32 && bufferSize!=64))
        continue;
    
    if (redSize==bitsNeeded && greenSize==bitsNeeded && blueSize==bitsNeeded)
    {
        printf("Selecting Config %02d: Pointer: %p, R=%d G=%d B=%d A=%d, Depth=%d, Stencil=%d, BufferSize=%d, SampleBuffers=%d, Samples=%d, SurfType=%d\n",
            i, (void*)configs[i], redSize, greenSize, blueSize, alphaSize, depthSize, stencilSize, bufferSize, sampleBuffers, samples, surfaceType);
        
        returnConfig = configs[i];
        break;
    }
    else
    {
        printf(" Skipping Config %02d: Pointer: %p, R=%d G=%d B=%d A=%d, Depth=%d, Stencil=%d, BufferSize=%d, SampleBuffers=%d, Samples=%d, SurfType=%d\n",
            i, (void*)configs[i], redSize, greenSize, blueSize, alphaSize, depthSize, stencilSize, bufferSize, sampleBuffers, samples, surfaceType);
    }
  }

  free(configs);
  return returnConfig;
}

static int init_gl(uint32_t fourCC)
{
    EGLint major, minor, n;
    GLuint vertex_shader, fragment_shader;
    GLint ret;

    if (!eglBindAPI(EGL_OPENGL_API)) {
        printf("failed to bind api EGL_OPENGL_API\n");
        return -1;
    }

    static const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display = NULL;
    get_platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress("eglGetPlatformDisplay");
    assert(get_platform_display != NULL);

    gl.display = get_platform_display(EGL_PLATFORM_GBM_KHR, gbm.dev, NULL);

    if (!eglInitialize(gl.display, &major, &minor)) {
        printf("failed to initialize\n");
        return -1;
    }

//	printf("EGL Extensions: %s\n", eglQueryString(gl.display, EGL_EXTENSIONS));

    // TEST_EGL_ANDROID_native_fence_sync
    eglCreateSyncKHR = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    eglDestroySyncKHR = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    eglDupNativeFenceFDANDROID = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");

    if (!eglCreateSyncKHR || !eglDestroySyncKHR || !eglDupNativeFenceFDANDROID) {
        printf("Failed to load EGL_ANDROID_native_fence_sync functions\n");
        exit(-1);
    }

    // END TEST_EGL_ANDROID_native_fence_sync

    eglSwapInterval(gl.display, 0);

/*
    printf("Using display %p with EGL version %d.%d\n",
            gl.display, major, minor);

    printf("EGL Version \"%s\"\n", eglQueryString(gl.display, EGL_VERSION));
    printf("EGL Vendor \"%s\"\n", eglQueryString(gl.display, EGL_VENDOR));
    printf("EGL Extensions \"%s\"\n", eglQueryString(gl.display, EGL_EXTENSIONS));
*/
    //GBM_FORMAT_XRGB2101010
    int bitsNeeded = -1;
    switch (fourCC)
    {
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_RGBX8888:
        case DRM_FORMAT_BGRX8888:
        case DRM_FORMAT_ARGB8888:
        case DRM_FORMAT_ABGR8888:
        case DRM_FORMAT_RGBA8888:
        case DRM_FORMAT_BGRA8888:
            bitsNeeded = 8;
            break;

        case DRM_FORMAT_XRGB2101010:
        case DRM_FORMAT_XBGR2101010:
        case DRM_FORMAT_RGBX1010102:
        case DRM_FORMAT_BGRX1010102:
        case DRM_FORMAT_ARGB2101010:
        case DRM_FORMAT_ABGR2101010:
        case DRM_FORMAT_RGBA1010102:
        case DRM_FORMAT_BGRA1010102:
            bitsNeeded = 10;
            break;

        case DRM_FORMAT_XRGB16161616:
        case DRM_FORMAT_XBGR16161616:
        case DRM_FORMAT_ARGB16161616:
        case DRM_FORMAT_ABGR16161616:
            bitsNeeded = 16;
            break;
    }
    if (bitsNeeded==-1)
    {		
        printf("unsupported fourcc: %d\n", fourCC);
        return -1;
    }

    gl.config = FindEGLConfig(gl.display,bitsNeeded);
    if (!gl.config)
    {
        printf("failed to find config for bitsNeeded: %d\n", bitsNeeded);
        return -1;
    }

    gl.context = eglCreateContext(gl.display, gl.config,
            EGL_NO_CONTEXT, context_attribs);
    if (gl.context == NULL) {
        printf("failed to create context\n");
        return -1;
    }

    gl.surface = eglCreateWindowSurface(gl.display, gl.config, gbm.surface, NULL);
    if (gl.surface == EGL_NO_SURFACE) {
        printf("failed to create egl surface\n");
        return -1;
    }

    /* connect the context to the surface */
    eglMakeCurrent(gl.display, gl.surface, gl.surface, gl.context);

    //printf("GL Extensions: \"%s\"\n", glGetString(GL_EXTENSIONS));

    return 0;
}

/* Draw code here */
static void draw(uint32_t i)
{
    glClear(GL_COLOR_BUFFER_BIT);
    static float v = 0.0;
    static float delta = 1.0/60.0;

    glClearColor(v,v,v,1);

    v += delta;	
    if (v>1.0 || v<0.0)
        delta = -delta;
}

static void
drm_fb_destroy_callback(struct gbm_bo *bo, void *data)
{
    struct drm_fb *fb = (drm_fb *)data;
    struct gbm_device *gbm = gbm_bo_get_device(bo);

    if (fb->fb_id)
        drmModeRmFB(drm.fd, fb->fb_id);

    free(fb);
}

static struct drm_fb * drm_fb_get_from_bo(struct gbm_bo *bo)
{
    struct drm_fb *fb = (drm_fb *)gbm_bo_get_user_data(bo);
    uint32_t width, height, stride, handle;
    int ret;

    if (fb)
        return fb;

    fb = (drm_fb *)calloc(1, sizeof *fb);
    fb->bo = bo;
    fb->state = READY_FOR_RENDER;

    width = gbm_bo_get_width(bo);
    height = gbm_bo_get_height(bo);
    stride = gbm_bo_get_stride(bo);
    handle = gbm_bo_get_handle(bo).u32;
#if 0
    ret = drmModeAddFB(drm.fd, width, height, 24, 32, stride, handle, &fb->fb_id);
    if (ret) {
        printf("failed to create fb: %s\n", strerror(errno));
        free(fb);
        return NULL;
    }
#else
    uint32_t handles[4] = {0};
    uint32_t pitches[4] = {0};
    uint32_t offsets[4] = {0};
    uint64_t modifiers[4] = {0};

    modifiers[0] = I915_FORMAT_MOD_4_TILED;
    handles[0] = handle;
    pitches[0] = stride;
    offsets[0] = 0;

    ret = drmModeAddFB2WithModifiers(drm.fd, width, height, DRM_FORMAT_XRGB8888, handles, pitches, offsets, modifiers, &fb->fb_id, DRM_MODE_FB_MODIFIERS);
    if (ret < 0) {
        printf("drmModeAddFB2WithModifiers");
        free(fb);
        return NULL;
    }
#endif

    gbm_bo_set_user_data(bo, fb, drm_fb_destroy_callback);

    return fb;
}


#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>

class PageFlipManager
{
public:
    PageFlipManager()
    :flipThread(nullptr),
    flipHandled(false),
    stopFlipThread(false),
    gbmSurface(NULL),
    eglSurface(EGL_NO_SURFACE),
    eglDisplay(EGL_NO_DISPLAY)
    {
        memset(drawBuffers,0,sizeof(drawBuffers));
    }

    void Init(gbm_surface *s, EGLDisplay eglDisplay, EGLSurface eglSurface);
    void RenderNextFrame();

protected:
    static const int NUM_BUFFERS=3;
    gbm_bo *drawBuffers[NUM_BUFFERS];

    gbm_surface *gbmSurface;
    EGLDisplay eglDisplay;
    EGLSurface eglSurface;

    class Queue {
    private:
        std::mutex mtx;
        std::condition_variable cv;
        std::queue<int> q;

    public:
        void push(int value) {
            {
                std::unique_lock<std::mutex> lock(mtx);
                q.push(value);
            }
            cv.notify_one();
        }

        int pop() {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [this]{ return !q.empty(); });
            int value = q.front();
            q.pop();
            return value;
        }
    };

    Queue freeQueue;
    Queue flipQueue;
    
    static void runFlipThread(PageFlipManager *tbm) { tbm->FlipThread(); }
    std::thread *flipThread;

    bool flipHandled;

    void FlipThread();
    static void page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *data);
    void PageFlipped(drm_fb *fb); //called from page_flip_handler in the flip thread

    bool stopFlipThread;	
};

PageFlipManager g_pageFlipManager;

void PageFlipManager::Init(gbm_surface *s, EGLDisplay _eglDisplay, EGLSurface _eglSurface)
{
    gbmSurface = s;
    eglDisplay = _eglDisplay;
    eglSurface = _eglSurface;

    glClearColor(0.5, 0.5, 0.5, 1.0);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(eglDisplay, eglSurface);
    gbm_bo *bo0 = gbm_surface_lock_front_buffer(gbmSurface);
    drm_fb *fb0 = drm_fb_get_from_bo(bo0);
    fb0->drawBufferIndex = 0;
    fb0->manager = this;

    glClearColor(0.5, 0.5, 0.5, 1.0);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(eglDisplay, eglSurface);
    gbm_bo *bo1 = gbm_surface_lock_front_buffer(gbmSurface);
    drm_fb *fb1 = drm_fb_get_from_bo(bo1);
    fb1->drawBufferIndex = 1;
    fb1->manager = this;

    glClearColor(0.5, 0.5, 0.5, 1.0);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(gl.display, gl.surface);
    gbm_bo *bo2 = gbm_surface_lock_front_buffer(gbm.surface);
    drm_fb *fb2 = drm_fb_get_from_bo(bo2);
    fb2->drawBufferIndex = 2;
    fb2->manager = this;

    /*
    glClearColor(0.5, 0.5, 0.5, 1.0);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(gl.display, gl.surface);
    gbm_bo *bo3 = gbm_surface_lock_front_buffer(gbm.surface);
    drm_fb *fb3 = drm_fb_get_from_bo(bo3);
    fb3->drawBufferIndex = 3;
    fb3->manager = this;
    gbm_surface_release_buffer(gbm.surface, bo3);
    drawBuffers[3] = bo3;
    */

    gbm_surface_release_buffer(gbm.surface, bo2);
    gbm_surface_release_buffer(gbm.surface, bo1);
    gbm_surface_release_buffer(gbm.surface, bo0);

    drawBuffers[0] = bo0;
    drawBuffers[1] = bo1;
    drawBuffers[2] = bo2;
    
    freeQueue.push(0);
    freeQueue.push(1);
    freeQueue.push(2);
    //freeQueue.push(3);

    // Start flip thread
    flipThread = new std::thread(runFlipThread,this);
}

void PageFlipManager::RenderNextFrame()
{
    auto startTime = std::chrono::high_resolution_clock::now();
        
    int drawBufferIndex = freeQueue.pop();
    //printf("RenderNextFrame: rendering to buffer: %d\n",drawBufferIndex);
    gbm_bo *draw_bo = drawBuffers[drawBufferIndex];
    drm_fb *fb = drm_fb_get_from_bo(draw_bo);
    if (fb->state!=READY_FOR_RENDER)
    {
        printf("RenderNextFrame: state!=READY_FOR_RENDER\n");
        exit(-1);
    }
    static int i=0;
    draw(i++);
    
    eglSwapBuffers(eglDisplay, eglSurface);


    // TEST_EGL_ANDROID_native_fence_sync

    // Create a native fence sync object
    EGLSyncKHR sync = eglCreateSyncKHR(eglDisplay, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
    if (sync == EGL_NO_SYNC_KHR) {
        printf("Failed to create EGL native fence sync: %d\n", eglGetError());
        exit(-1);
    }

    // Wait for the fence to signal completion
    int fenceFd = eglDupNativeFenceFDANDROID(eglDisplay, sync);
    if (fenceFd == EGL_NO_NATIVE_FENCE_FD_ANDROID) {
        printf("Failed to duplicate native fence FD: %d\n", eglGetError());
        eglDestroySyncKHR(eglDisplay, sync);
        exit(-1);
    }

    // Close the fence FD after use
    close(fenceFd);

    // Destroy the sync object
    eglDestroySyncKHR(eglDisplay, sync);

    // END TEST_EGL_ANDROID_native_fence_sync


    gbm_bo *front_bo = gbm_surface_lock_front_buffer(gbmSurface);
    if (front_bo!=draw_bo)
    {
        printf("front_bo != draw_bo\n");
        exit(-1);
    }
    fb->state = QUEUED_FOR_FLIP;

    auto endTime = std::chrono::high_resolution_clock::now();
    auto elapsed = endTime-startTime;
    auto t = std::chrono::duration_cast<std::chrono::duration<double>>(elapsed);
        
    flipQueue.push(drawBufferIndex);
    //printf("RenderNextFrame: passed buffer to flip thread: %d - elapsed time: %2.2lfms\n",drawBufferIndex,t.count()*1000.0);
}

void PageFlipManager::page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *data)
{
    drm_fb *fb = (drm_fb *)data;
    PageFlipManager *pfm = fb->manager;
    pfm->PageFlipped(fb);
}

void PageFlipManager::PageFlipped(drm_fb *fb)
{
    //this buffer is flipped and visible now
    fb->state = FLIPPED_AND_VISIBLE;

    //we know that the buffer just before this one in our ring-buffer was the previous one visible. it can now be released and drawn to
    int releaseBufferIndex = (fb->drawBufferIndex+(NUM_BUFFERS-1)) % NUM_BUFFERS;
    struct gbm_bo *release_bo = drawBuffers[releaseBufferIndex];
    struct drm_fb *release_fb = drm_fb_get_from_bo(release_bo);
    if (release_fb->state==FLIPPED_AND_VISIBLE) //very first few flips, previous frames will not have been flipped/visible yet until the first cycle through the ring buffer is completed
    {
        gbm_surface_release_buffer(gbm.surface, release_bo);
        release_fb->state = READY_FOR_RENDER;
        
        //printf("     FlipThread: buffer flipped and visible: %d - releasing buffer for rendering: %d\n", fb->drawBufferIndex, releaseBufferIndex);

        // After flip completion, queue the buffer as free
        freeQueue.push(releaseBufferIndex);
    }
    else
    {
        //printf("     FlipThread: buffer flipped and visible: %d - no buffer to release for rendering\n", fb->drawBufferIndex);
    }

    flipHandled = true;
}

void PageFlipManager::FlipThread() {

    drmEventContext evctx = {
            .version = DRM_EVENT_CONTEXT_VERSION,
            .page_flip_handler = page_flip_handler,
    };

    while (stopFlipThread==false) {
        // Wait for a buffer to flip
        int flipBufferIndex = flipQueue.pop();

        gbm_bo *flip_bo = drawBuffers[flipBufferIndex];
        drm_fb *fb = drm_fb_get_from_bo(flip_bo);

        //printf("     FlipThread: calling drmModePageFlip for buffer: %d\n", fb->drawBufferIndex);
        
        int ret = drmModePageFlip(drm.fd, drm.crtc_id, fb->fb_id, DRM_MODE_PAGE_FLIP_EVENT, fb);
        if (ret) {
            auto startTime = std::chrono::high_resolution_clock::now();		
            std::chrono::duration<double> waitTime;
            int numRetries = 0;
            
            while (ret)
            {
                if ((-ret)!=EBUSY)
                {
                    printf("     FlipThread: drmModePageFlip error on buffer %d: %s\n", fb->drawBufferIndex, strerror(errno));
                    exit(-1);
                }

                ret = drmModePageFlip(drm.fd, drm.crtc_id, fb->fb_id, DRM_MODE_PAGE_FLIP_EVENT, fb);
                numRetries++;
                auto endTime = std::chrono::high_resolution_clock::now();
                auto elapsed = endTime-startTime;
                waitTime = std::chrono::duration_cast<std::chrono::duration<double>>(elapsed);
                if (waitTime.count()>1.0)
                {
                    printf("     FlipThread: drmModePageFlip timed out for buffer %d: %s\n", fb->drawBufferIndex, strerror(errno));
                    exit(-1);
                }
            }
        
            printf("     FlipThread: drmModePageFlip %d retries for buffer %d took extra %2.2lfms\n", numRetries, (int)fb->drawBufferIndex, waitTime.count()*1000.0);
        }

        //process drm events via drmHandleEvent until we've handle a page flip event
        flipHandled = false;
        while (!flipHandled)
        {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(0, &fds);
            FD_SET(drm.fd, &fds);
            ret = select(drm.fd + 1, &fds, NULL, NULL, NULL);
            if (ret < 0) {
                printf("select err: %s\n", strerror(errno));
                //return ret;
            } else if (ret == 0) {
                printf("select timeout!\n");
                //return -1;
            } else if (FD_ISSET(0, &fds)) {
                printf("user interrupted!\n");
                //break;
            }
            drmHandleEvent(drm.fd, &evctx);
        }
    }
}

#include <string>

int main(int argc, char *argv[])
{
    // Default resolution is 8K (7680x4320 @ 60 Hz)
    int desiredWidth = 7680;
    int desiredHeight = 4320;
    int desiredFPS = 60;

    // Parse command-line arguments
    if (argc > 1) {
        std::string mode = argv[1];

        if (mode == "2k60") {
            desiredWidth = 1920;
            desiredHeight = 1080;
            desiredFPS = 60;
        } else if (mode == "4k60") {
            desiredWidth = 3840;
            desiredHeight = 2160;
            desiredFPS = 60;
        } else if (mode == "8k60") {
            desiredWidth = 7680;
            desiredHeight = 4320;
            desiredFPS = 60;
        } else {
            printf("Invalid mode specified: %s\n", mode.c_str());
            printf("Usage: %s [2k60|4k60|8k60]\n", argv[0]);
            return -1;
        }
    }

    // Print selected resolution
    printf("Selected resolution: %dx%d @ %d FPS\n", desiredWidth, desiredHeight, desiredFPS);

    // Initialize DRM
    int ret = init_drm(desiredWidth, desiredHeight, desiredFPS);
    if (ret) {
        printf("Failed to initialize DRM\n");
        return ret;
    }

    // Initialize GBM
    ret = init_gbm(DRM_FORMAT_XRGB8888);
    if (ret) {
        printf("Failed to initialize GBM\n");
        return ret;
    }

    // Initialize EGL
    ret = init_gl(DRM_FORMAT_XRGB8888);
    if (ret) {
        printf("Failed to initialize EGL\n");
        return ret;
    }

    // Clear the color buffer
    glClearColor(0.5, 0.5, 0.5, 1.0);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(gl.display, gl.surface);

    struct gbm_bo *bo0 = gbm_surface_lock_front_buffer(gbm.surface);
    struct drm_fb *fb = drm_fb_get_from_bo(bo0);

    // Set mode
    double fps = (double)(drm.mode->clock * 1000.0) / (double)(drm.mode->htotal * drm.mode->vtotal);
    printf("drmModeSetCrtc with size: %dx%dx%2.2lf\n", (int)drm.mode->hdisplay, (int)drm.mode->vdisplay, fps);

    ret = drmModeSetCrtc(drm.fd, drm.crtc_id, fb->fb_id, 0, 0, &drm.connector_id, 1, drm.mode);
    if (ret) {
        printf("Failed to set mode: %s\n", strerror(errno));
        return ret;
    }

    gbm_surface_release_buffer(gbm.surface, bo0);

    // Initialize page flip manager
    PageFlipManager pageFlipManager;
    pageFlipManager.Init(gbm.surface, gl.display, gl.surface);

    // Main loop
    while (1) {
        pageFlipManager.RenderNextFrame();
    }

    return ret;
}
