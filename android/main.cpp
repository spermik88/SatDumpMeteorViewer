#include <unistd.h>
#include <android/log.h>
#include <android/asset_manager.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <stdexcept>
#include <utility>
#include "backend.h"
#include "main_ui.h"
#include "logger.h"
#include "init.h"
#include "loader/loader.h"
#include "imgui/imgui_internal.h"

// Data
EGLDisplay g_EglDisplay = EGL_NO_DISPLAY;
EGLSurface g_EglSurface = EGL_NO_SURFACE;
static EGLContext g_EglContext = EGL_NO_CONTEXT;
static bool g_Initialized = false;
static char g_LogTag[] = "SatDump";
extern struct android_app *g_App;
extern std::string android_plugins_dir;
bool was_init = false;

// Forward declarations of helper functions
static int ShowSoftKeyboardInput();
static int HideSoftKeyboardInput();
static int PollUnicodeChars();
static int GetAssetData(const char *filename, void **out_data);
void bindImageTextureFunctions();

namespace
{
    class ScopedJniEnv
    {
    public:
        explicit ScopedJniEnv(JavaVM *vm) : vm(vm)
        {
            if (vm == nullptr)
                throw std::runtime_error("Java VM is unavailable");

            jint result = vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
            if (result == JNI_OK)
                return;
            if (result != JNI_EDETACHED || vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
                throw std::runtime_error("Could not attach to JNI environment");
            attached_by_scope = true;
        }

        ~ScopedJniEnv()
        {
            if (attached_by_scope)
                vm->DetachCurrentThread();
        }

        JNIEnv *get() const { return env; }

    private:
        JavaVM *vm = nullptr;
        JNIEnv *env = nullptr;
        bool attached_by_scope = false;
    };

    void throw_if_java_exception(JNIEnv *env, const char *context)
    {
        if (!env->ExceptionCheck())
            return;
        env->ExceptionDescribe();
        env->ExceptionClear();
        throw std::runtime_error(context);
    }

    std::string call_activity_string_method(struct android_app *app, const char *method_name)
    {
        ScopedJniEnv scope(app->activity->vm);
        JNIEnv *env = scope.get();
        jclass clazz = env->GetObjectClass(app->activity->clazz);
        if (clazz == nullptr)
            throw std::runtime_error("Could not get MainActivity class");
        jmethodID method = env->GetMethodID(clazz, method_name, "()Ljava/lang/String;");
        if (method == nullptr)
        {
            env->DeleteLocalRef(clazz);
            throw std::runtime_error("Could not get MainActivity method");
        }

        jstring value = static_cast<jstring>(env->CallObjectMethod(app->activity->clazz, method));
        throw_if_java_exception(env, "MainActivity string method failed");
        if (value == nullptr)
        {
            env->DeleteLocalRef(clazz);
            throw std::runtime_error("MainActivity returned no path");
        }
        const char *raw = env->GetStringUTFChars(value, nullptr);
        if (raw == nullptr)
        {
            env->DeleteLocalRef(value);
            env->DeleteLocalRef(clazz);
            throw_if_java_exception(env, "Could not read MainActivity result");
            throw std::runtime_error("Could not read MainActivity result");
        }
        std::string result(raw);
        env->ReleaseStringUTFChars(value, raw);
        env->DeleteLocalRef(value);
        env->DeleteLocalRef(clazz);
        return result;
    }
}

void init(struct android_app *app)
{
    setenv("LIBUSB_ANDROID_JVM_PTR", std::to_string((size_t)app).c_str(), true);

    if (g_Initialized)
        return;

    g_App = app;
    if (g_App == nullptr || g_App->window == nullptr)
    {
        __android_log_print(ANDROID_LOG_ERROR, g_LogTag, "%s", "Cannot initialize: invalid android app window");
        return;
    }
    ANativeWindow_acquire(g_App->window);

    auto cleanup_failed_init = []()
    {
        if (g_EglDisplay != EGL_NO_DISPLAY)
        {
            eglMakeCurrent(g_EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (g_EglContext != EGL_NO_CONTEXT)
                eglDestroyContext(g_EglDisplay, g_EglContext);
            if (g_EglSurface != EGL_NO_SURFACE)
                eglDestroySurface(g_EglDisplay, g_EglSurface);
            eglTerminate(g_EglDisplay);
        }
        g_EglDisplay = EGL_NO_DISPLAY;
        g_EglContext = EGL_NO_CONTEXT;
        g_EglSurface = EGL_NO_SURFACE;
        if (g_App != nullptr && g_App->window != nullptr)
            ANativeWindow_release(g_App->window);
    };

    // Initialize EGL
    // This is mostly boilerplate code for EGL...
    {
        g_EglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (g_EglDisplay == EGL_NO_DISPLAY)
        {
            __android_log_print(ANDROID_LOG_ERROR, g_LogTag, "%s", "eglGetDisplay(EGL_DEFAULT_DISPLAY) returned EGL_NO_DISPLAY");
            cleanup_failed_init();
            return;
        }

        if (eglInitialize(g_EglDisplay, 0, 0) != EGL_TRUE)
        {
            __android_log_print(ANDROID_LOG_ERROR, g_LogTag, "%s", "eglInitialize() returned with an error");
            cleanup_failed_init();
            return;
        }

        const EGLint egl_attributes[] = {EGL_BLUE_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_RED_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE};
        EGLint num_configs = 0;
        if (eglChooseConfig(g_EglDisplay, egl_attributes, nullptr, 0, &num_configs) != EGL_TRUE)
        {
            __android_log_print(ANDROID_LOG_ERROR, g_LogTag, "%s", "eglChooseConfig() returned with an error");
            cleanup_failed_init();
            return;
        }
        if (num_configs == 0)
        {
            __android_log_print(ANDROID_LOG_ERROR, g_LogTag, "%s", "eglChooseConfig() returned 0 matching config");
            cleanup_failed_init();
            return;
        }

        // Get the first matching config
        EGLConfig egl_config;
        if (eglChooseConfig(g_EglDisplay, egl_attributes, &egl_config, 1, &num_configs) != EGL_TRUE || num_configs <= 0)
        {
            __android_log_print(ANDROID_LOG_ERROR, g_LogTag, "%s", "eglChooseConfig() failed to get a valid config");
            cleanup_failed_init();
            return;
        }
        EGLint egl_format;
        eglGetConfigAttrib(g_EglDisplay, egl_config, EGL_NATIVE_VISUAL_ID, &egl_format);
        ANativeWindow_setBuffersGeometry(g_App->window, 0, 0, egl_format);

        const EGLint egl_context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        g_EglContext = eglCreateContext(g_EglDisplay, egl_config, EGL_NO_CONTEXT, egl_context_attributes);

        if (g_EglContext == EGL_NO_CONTEXT)
        {
            __android_log_print(ANDROID_LOG_ERROR, g_LogTag, "%s", "eglCreateContext() returned EGL_NO_CONTEXT");
            cleanup_failed_init();
            return;
        }

        g_EglSurface = eglCreateWindowSurface(g_EglDisplay, egl_config, g_App->window, NULL);
        if (g_EglSurface == EGL_NO_SURFACE)
        {
            __android_log_print(ANDROID_LOG_ERROR, g_LogTag, "%s", "eglCreateWindowSurface() returned EGL_NO_SURFACE");
            cleanup_failed_init();
            return;
        }
        if (eglMakeCurrent(g_EglDisplay, g_EglSurface, g_EglSurface, g_EglContext) != EGL_TRUE)
        {
            __android_log_print(ANDROID_LOG_ERROR, g_LogTag, "%s", "eglMakeCurrent() returned with an error");
            cleanup_failed_init();
            return;
        }
    }

    if (ImGui::GetCurrentContext() == nullptr)
    {
        // Setup Dear ImGui context
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
    }
    ImGuiIO &io = ImGui::GetIO();

    if (!was_init)
    {
        // Disable loading/saving of .ini file from disk.
        // FIXME: Consider using LoadIniSettingsFromMemory() / SaveIniSettingsToMemory() to save in appropriate location for Android.
        io.IniFilename = NULL;
    }

    // Setup Platform/Renderer backends
    if (!ImGui_ImplAndroid_Init(g_App->window))
    {
        __android_log_print(ANDROID_LOG_ERROR, g_LogTag, "%s", "ImGui_ImplAndroid_Init() failed");
        cleanup_failed_init();
        return;
    }
    if (!ImGui_ImplOpenGL3_Init("#version 300 es"))
    {
        __android_log_print(ANDROID_LOG_ERROR, g_LogTag, "%s", "ImGui_ImplOpenGL3_Init() failed");
        ImGui_ImplAndroid_Shutdown();
        cleanup_failed_init();
        return;
    }

    if (!was_init)
    {

        // Setup Dear ImGui style
        // ImGui::StyleColorsDark();
        // ImGui::StyleColorsClassic();

        initLogger();
        style::setFonts(backend::device_scale);
        HideSoftKeyboardInput();
        eglSwapInterval(g_EglDisplay, 0);
        std::shared_ptr<satdump::LoadingScreenSink> loading_screen_sink = std::make_shared<satdump::LoadingScreenSink>();
        logger->add_sink(loading_screen_sink);

        satdump::tle_do_update_on_init = false;
        satdump::initSatdump();
        satdump::initMainUI();

        //Shut down loading screen
        logger->del_sink(loading_screen_sink);
        loading_screen_sink.reset();

        //Set font again to adjust for DPI
        eglSwapInterval(g_EglDisplay, 1);

        was_init = true;
    }
    else
        HideSoftKeyboardInput();

    g_Initialized = true;
}

void tick()
{
    if (!g_Initialized || g_EglDisplay == EGL_NO_DISPLAY || g_EglSurface == EGL_NO_SURFACE || g_EglContext == EGL_NO_CONTEXT)
        return;

    if (ImGui::GetCurrentContext() == nullptr)
        return;

    ImGuiIO &io = ImGui::GetIO();

    // Poll Unicode characters via JNI
    // FIXME: do not call this every frame because of JNI overhead
    PollUnicodeChars();

    // Open on-screen (soft) input if requested by Dear ImGui
    static bool WantTextInputLast = false;
    if (io.WantTextInput && !WantTextInputLast)
        ShowSoftKeyboardInput();
    if (!io.WantTextInput && WantTextInputLast)
        HideSoftKeyboardInput();
    WantTextInputLast = io.WantTextInput;

    // Rendering
    satdump::renderMainUI();
}

void shutdown()
{
    if (!g_Initialized)
        return;

    // A USB permission activity can take the native window between NewFrame()
    // and Render().  Finish that frame before tearing down the backends so a
    // later APP_CMD_INIT_WINDOW does not trip ImGui's frame sanity assertion.
    ImGuiContext *context = ImGui::GetCurrentContext();
    if (context != nullptr && context->WithinFrameScope)
        ImGui::EndFrame();

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplAndroid_Shutdown();
    // ImGui::DestroyContext();

    if (g_EglDisplay != EGL_NO_DISPLAY)
    {
        eglMakeCurrent(g_EglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

        if (g_EglContext != EGL_NO_CONTEXT)
            eglDestroyContext(g_EglDisplay, g_EglContext);

        if (g_EglSurface != EGL_NO_SURFACE)
            eglDestroySurface(g_EglDisplay, g_EglSurface);

        eglTerminate(g_EglDisplay);
    }

    g_EglDisplay = EGL_NO_DISPLAY;
    g_EglContext = EGL_NO_CONTEXT;
    g_EglSurface = EGL_NO_SURFACE;
    ANativeWindow_release(g_App->window);

    g_Initialized = false;
}

static void handleAppCmd(struct android_app *app, int32_t appCmd)
{
    switch (appCmd)
    {
    case APP_CMD_INIT_WINDOW:
        init(app);
        break;
    case APP_CMD_TERM_WINDOW:
        shutdown();
        break;
    case APP_CMD_PAUSE:
        HideSoftKeyboardInput();
        break;
    case APP_CMD_SAVE_STATE:
        satdump::recorder_app->save_settings();
        satdump::viewer_app->save_settings();
        satdump::config::saveUserConfig();
        break;
    }
}

static int32_t handleInputEvent(struct android_app *app, AInputEvent *inputEvent)
{
    (void)app;

    if (AInputEvent_getType(inputEvent) == AINPUT_EVENT_TYPE_KEY &&
        AKeyEvent_getKeyCode(inputEvent) == AKEYCODE_BACK)
    {
        // Viewer -> Archive. Archive keeps default Android back behavior (exit).
        if (satdump::current_screen == satdump::Screen::Viewer)
        {
            if (AKeyEvent_getAction(inputEvent) == AKEY_EVENT_ACTION_UP)
                satdump::current_screen = satdump::Screen::Archive;
            return 1;
        }
        return 0;
    }

    return ImGui_ImplAndroid_HandleInputEvent(inputEvent);
}

std::string getAppFilesDir(struct android_app *app)
{
    return call_activity_string_method(app, "getAppDir");
}

std::string getPluginsDir(struct android_app *app)
{
    return call_activity_string_method(app, "get_plugins_directory");
}

void android_main(struct android_app *app)
{
    g_App = app;
    bindImageTextureFunctions();
    bindBackendFunctions();

    std::string path;
    try
    {
        path = getAppFilesDir(app);
        android_plugins_dir = getPluginsDir(app);
    }
    catch (const std::exception &e)
    {
        __android_log_print(ANDROID_LOG_ERROR, g_LogTag, "Android startup failed: %s", e.what());
        return;
    }
    if (path.empty())
    {
        __android_log_print(ANDROID_LOG_ERROR, g_LogTag, "%s", "Android startup aborted because app files are unavailable");
        return;
    }
    chdir(path.c_str());

    app->onAppCmd = handleAppCmd;
    app->onInputEvent = handleInputEvent;

    while (true)
    {
        int out_events;
        struct android_poll_source *out_data;

        // Poll all events. If the app is not visible, this loop blocks until g_Initialized == true.
        while (ALooper_pollAll(g_Initialized ? 0 : -1, NULL, &out_events, (void **)&out_data) >= 0)
        {
            // Process one event
            if (out_data != NULL)
                out_data->process(app, out_data);

            // Exit the app by returning from within the infinite loop
            if (app->destroyRequested != 0)
            {
                // shutdown() should have been called already while processing the
                // app command APP_CMD_TERM_WINDOW. But we play save here
                if (g_Initialized)
                    shutdown();

                return;
            }
        }

        // Initiate a new frame
        tick();
    }
}

// Unfortunately, there is no way to show the on-screen input from native code.
// Therefore, we call ShowSoftKeyboardInput() of the main activity implemented in MainActivity.kt via JNI.
static int ShowSoftKeyboardInput()
{
    try
    {
        ScopedJniEnv scope(g_App->activity->vm);
        JNIEnv *env = scope.get();
        jclass clazz = env->GetObjectClass(g_App->activity->clazz);
        if (clazz == nullptr)
            throw std::runtime_error("Could not get MainActivity class");
        jmethodID method = env->GetMethodID(clazz, "showSoftInput", "()V");
        if (method == nullptr)
        {
            env->DeleteLocalRef(clazz);
            throw std::runtime_error("Could not get showSoftInput method");
        }
        env->CallVoidMethod(g_App->activity->clazz, method);
        throw_if_java_exception(env, "showSoftInput failed");
        env->DeleteLocalRef(clazz);
        return 0;
    }
    catch (const std::exception &e)
    {
        __android_log_print(ANDROID_LOG_WARN, g_LogTag, "showSoftInput failed: %s", e.what());
        return -1;
    }
}

static int HideSoftKeyboardInput()
{
    try
    {
        ScopedJniEnv scope(g_App->activity->vm);
        JNIEnv *env = scope.get();
        jclass clazz = env->GetObjectClass(g_App->activity->clazz);
        if (clazz == nullptr)
            throw std::runtime_error("Could not get MainActivity class");
        jmethodID method = env->GetMethodID(clazz, "hideSoftInput", "()V");
        if (method == nullptr)
        {
            env->DeleteLocalRef(clazz);
            throw std::runtime_error("Could not get hideSoftInput method");
        }
        env->CallVoidMethod(g_App->activity->clazz, method);
        throw_if_java_exception(env, "hideSoftInput failed");
        env->DeleteLocalRef(clazz);
        return 0;
    }
    catch (const std::exception &e)
    {
        __android_log_print(ANDROID_LOG_WARN, g_LogTag, "hideSoftInput failed: %s", e.what());
        return -1;
    }
}

// Unfortunately, the native KeyEvent implementation has no getUnicodeChar() function.
// Therefore, we implement the processing of KeyEvents in MainActivity.kt and poll
// the resulting Unicode characters here via JNI and send them to Dear ImGui.
static int PollUnicodeChars()
{
    try
    {
        ScopedJniEnv scope(g_App->activity->vm);
        JNIEnv *env = scope.get();
        jclass clazz = env->GetObjectClass(g_App->activity->clazz);
        if (clazz == nullptr)
            throw std::runtime_error("Could not get MainActivity class");
        jmethodID method = env->GetMethodID(clazz, "pollUnicodeChar", "()I");
        if (method == nullptr)
        {
            env->DeleteLocalRef(clazz);
            throw std::runtime_error("Could not get pollUnicodeChar method");
        }

        ImGuiIO &io = ImGui::GetIO();
        jint unicode_character;
        while ((unicode_character = env->CallIntMethod(g_App->activity->clazz, method)) != 0)
        {
            throw_if_java_exception(env, "pollUnicodeChar failed");
            if (unicode_character == 0x08) // BackSpace
            {
                io.AddKeyEvent(ImGuiKey_Backspace, true);
                io.AddKeyEvent(ImGuiKey_Backspace, false);
            }
            else
                io.AddInputCharacter(unicode_character);
        }
        throw_if_java_exception(env, "pollUnicodeChar failed");
        env->DeleteLocalRef(clazz);
        return 0;
    }
    catch (const std::exception &e)
    {
        __android_log_print(ANDROID_LOG_WARN, g_LogTag, "pollUnicodeChar failed: %s", e.what());
        return -1;
    }
}

// Helper to retrieve data placed into the assets/ directory (android/app/src/main/assets)
static int GetAssetData(const char *filename, void **outData)
{
    int num_bytes = 0;
    AAsset *asset_descriptor = AAssetManager_open(g_App->activity->assetManager, filename, AASSET_MODE_BUFFER);
    if (asset_descriptor)
    {
        num_bytes = AAsset_getLength(asset_descriptor);
        *outData = IM_ALLOC(num_bytes);
        int64_t num_bytes_read = AAsset_read(asset_descriptor, *outData, num_bytes);
        AAsset_close(asset_descriptor);
        IM_ASSERT(num_bytes_read == num_bytes);
    }
    return num_bytes;
}
