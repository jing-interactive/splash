#include "./core/scene.h"

#include <utility>

#include "./controller/controller_blender.h"
#include "./controller/controller_gui.h"
#include "./core/link.h"
#include "./graphics/camera.h"
#include "./graphics/filter.h"
#include "./graphics/geometry.h"
#include "./graphics/object.h"
#include "./graphics/profiler_gl.h"
#include "./graphics/texture.h"
#include "./graphics/texture_image.h"
#include "./graphics/warp.h"
#include "./graphics/window.h"
#include "./image/image.h"
#include "./image/queue.h"
#include "./mesh/mesh.h"
#include "./userinput/userinput_dragndrop.h"
#include "./userinput/userinput_joystick.h"
#include "./userinput/userinput_keyboard.h"
#include "./userinput/userinput_mouse.h"
#include "./utils/log.h"
#include "./utils/osutils.h"
#include "./utils/timer.h"

#if HAVE_GPHOTO
#include "./controller/colorcalibrator.h"
#endif

#if HAVE_OSX
#include "./graphics/texture_syphon.h"
#else
// clang-format off
#define GLFW_EXPOSE_NATIVE_X11
#define GLFW_EXPOSE_NATIVE_GLX
#include <GLFW/glfw3native.h>
#include <GL/glxext.h>
// clang-format on
#endif

using namespace std;

namespace Splash
{

bool Scene::_hasNVSwapGroup{false};
vector<int> Scene::_glVersion{0, 0};
vector<string> Scene::_ghostableTypes{"camera", "warp"};

/*************/
std::vector<int> Scene::getGLVersion()
{
    return _glVersion;
}

/*************/
bool Scene::getHasNVSwapGroup()
{
    return _hasNVSwapGroup;
}

/*************/
Scene::Scene(const string& name, const string& socketPrefix)
{
    Log::get() << Log::DEBUGGING << "Scene::Scene - Scene created successfully" << Log::endl;

    _isRunning = true;
    _name = name;
    _linkSocketPrefix = socketPrefix;

    // We have to reset the factory to create a Scene factory
    _factory.reset(new Factory(this));
    _blender = make_shared<Blender>(this);
    if (_blender)
    {
        _blender->setName("blender");
        _objects["blender"] = _blender;
    }

    registerAttributes();

    init(_name);
}

/*************/
Scene::~Scene()
{
    // Cleanup every object
    _mainWindow->setAsCurrentContext();
    lock_guard<recursive_mutex> lockObjects(_objectsMutex); // We don't want any friend to try accessing the objects

    // Free objects cleanly
    for (auto& obj : _objects)
        obj.second.reset();
    _objects.clear();
    for (auto& obj : _objects)
        obj.second.reset();

    _mainWindow->releaseContext();

    _link->disconnectFrom("world");

    Log::get() << Log::DEBUGGING << "Scene::~Scene - Destructor" << Log::endl;
}

/*************/
std::shared_ptr<GraphObject> Scene::addObject(const string& type, const string& name)
{
    Log::get() << Log::DEBUGGING << "Scene::" << __FUNCTION__ << " - Creating object of type " << type << Log::endl;

    lock_guard<recursive_mutex> lockObjects(_objectsMutex);

    // If we run in background mode, don't create any window
    if (_runInBackground && type == "window")
        return {};

    // Check whether an object of this name already exists
    if (getObject(name))
    {
        Log::get() << Log::DEBUGGING << "Scene::" << __FUNCTION__ << " - An object named " << name << " already exists" << Log::endl;
        return {};
    }

    // Create the wanted object
    auto obj = _factory->create(type);

    // Add the object to the objects list
    if (obj.get() != nullptr)
    {
        obj->setRemoteType(type); // Not all objects have remote types, but this doesn't harm

        obj->setName(name);
        _objects[name] = obj;

        // Some objects have to be connected to the gui (if the Scene is master)
        if (_gui != nullptr)
        {
            if (obj->getType() == "object")
                link(obj, dynamic_pointer_cast<GraphObject>(_gui));
            else if (obj->getType() == "window" && !_guiLinkedToWindow)
            {
                link(dynamic_pointer_cast<GraphObject>(_gui), obj);
                _guiLinkedToWindow = true;
            }
        }
    }

    return obj;
}

/*************/
void Scene::addGhost(const string& type, const string& name)
{
    // Currently, only Cameras can be ghosts
    if (find(_ghostableTypes.begin(), _ghostableTypes.end(), type) == _ghostableTypes.end())
        return;

    Log::get() << Log::DEBUGGING << "Scene::" << __FUNCTION__ << " - Creating ghost object of type " << type << Log::endl;

    // Add the object for real ...
    auto obj = addObject(type, name);
    if (obj)
    {
        // And move it to _objects
        lock_guard<recursive_mutex> lockObjects(_objectsMutex);
        obj->setGhost(true);
    }
}

/*************/
Values Scene::getAttributeFromObject(const string& name, const string& attribute)
{
    lock_guard<recursive_mutex> lockObjects(_objectsMutex);
    auto object = getObject(name);

    Values values;
    if (object)
    {
        object->getAttribute(attribute, values);
    }
    // Ask the World if it knows more about this object
    else
    {
        auto answer = sendMessageToWorldWithAnswer("getAttribute", {name, attribute}, 1e4);
        for (unsigned int i = 1; i < answer.size(); ++i)
            values.push_back(answer[i]);
    }

    return values;
}

/*************/
Values Scene::getAttributeDescriptionFromObject(const string& name, const string& attribute)
{
    lock_guard<recursive_mutex> lockObjects(_objectsMutex);
    auto objectIt = _objects.find(name);

    Values values;
    if (objectIt != _objects.end())
    {
        auto& object = objectIt->second;
        values.push_back(object->getAttributeDescription(attribute));
    }

    // Ask the World if it knows more about this object
    if (values.size() == 0 || values[0].as<string>() == "")
    {
        auto answer = sendMessageToWorldWithAnswer("getAttributeDescription", {name, attribute}, 10000);
        if (!answer.empty())
        {
            values.clear();
            values.push_back(answer[1]);
        }
    }

    return values;
}

/*************/
Json::Value Scene::getConfigurationAsJson()
{
    lock_guard<recursive_mutex> lockObjects(_objectsMutex);

    Json::Value root;
    auto sceneConfiguration = BaseObject::getConfigurationAsJson();
    for (const auto& attr : sceneConfiguration.getMemberNames())
        root[attr] = sceneConfiguration[attr];

    // Save objects attributes
    for (auto& obj : _objects)
        if (obj.second->getSavable() && !obj.second->isGhost())
            root["objects"][obj.first] = obj.second->getConfigurationAsJson();

    // Save links
    Values links;
    for (auto& obj : _objects)
    {
        if (!obj.second->getSavable() || obj.second->isGhost())
            continue;

        auto linkedObjects = obj.second->getLinkedObjects();
        for (auto& linkedObj : linkedObjects)
        {
            if (!linkedObj->getSavable() || obj.second->isGhost())
                continue;

            links.push_back(Values({linkedObj->getName(), obj.second->getName()}));
        }
    }

    root["links"] = getValuesAsJson(links);

    return root;
}

/*************/
bool Scene::link(const string& first, const string& second)
{
    return link(getObject(first), getObject(second));
}

/*************/
bool Scene::link(const std::shared_ptr<GraphObject>& first, const std::shared_ptr<GraphObject>& second)
{
    if (!first || !second)
        return false;

    lock_guard<recursive_mutex> lockObjects(_objectsMutex);
    bool result = second->linkTo(first);

    return result;
}

/*************/
void Scene::unlink(const string& first, const string& second)
{
    unlink(getObject(first), getObject(second));
}

/*************/
void Scene::unlink(const std::shared_ptr<GraphObject>& first, const std::shared_ptr<GraphObject>& second)
{
    if (!first || !second)
        return;

    second->unlinkFrom(first);
}

/*************/
void Scene::remove(const string& name)
{
    std::shared_ptr<GraphObject> obj;

    lock_guard<recursive_mutex> lockObjects(_objectsMutex);

    if (_objects.find(name) != _objects.end())
        _objects.erase(name);
}

/*************/
void Scene::render()
{
    {
#ifdef PROFILE
        PROFILEGL("Render loop")
#endif
        // Create lists of objects to update and to render
        map<GraphObject::Priority, vector<shared_ptr<GraphObject>>> objectList{};
        {
            lock_guard<recursive_mutex> lockObjects(_objectsMutex);
            for (auto& obj : _objects)
            {
                // We also run all pending tasks for every object
                obj.second->runTasks();

                // Ghosts are not updated in the render loop
                if (obj.second->isGhost())
                    continue;

                auto priority = obj.second->getRenderingPriority();
                if (priority == GraphObject::Priority::NO_RENDER)
                    continue;

                auto listIt = objectList.find(priority);
                if (listIt == objectList.end())
                {
                    auto entry = objectList.emplace(std::make_pair(priority, vector<shared_ptr<GraphObject>>()));
                    if (entry.second == true)
                        listIt = entry.first;
                    else
                        continue;
                }

                listIt->second.push_back(obj.second);
            }
        }

        // Update and render the objects
        // See GraphObject::getRenderingPriority() for precision about priorities
        bool firstTextureSync = true; // Sync with the texture upload the first time we need textures
        bool firstWindowSync = true;  // Sync with the texture upload the last time we need textures
        auto textureLock = unique_lock<Spinlock>(_textureMutex, defer_lock);
        for (auto& objPriority : objectList)
        {
            // If the objects needs some Textures, we need to sync
            if (firstTextureSync && objPriority.first > GraphObject::Priority::BLENDING && objPriority.first < GraphObject::Priority::POST_CAMERA)
            {
#ifdef PROFILE
                PROFILEGL("texture upload lock");
#endif
                // We wait for textures to be uploaded, and we prevent any upload while rendering
                // cameras to prevent tearing
                textureLock.lock();
                if (glIsSync(_textureUploadFence) == GL_TRUE)
                {
                    glWaitSync(_textureUploadFence, 0, GL_TIMEOUT_IGNORED);
                    glDeleteSync(_textureUploadFence);
                }
                firstTextureSync = false;
            }

            if (objPriority.second.size() != 0)
                Timer::get() << objPriority.second[0]->getType();

            for (auto& obj : objPriority.second)
            {
#ifdef PROFILE
                PROFILEGL("object " + obj->getName());
#endif
                obj->update();

                auto objectCategory = obj->getCategory();
                if (objectCategory == GraphObject::Category::MESH)
                    if (obj->wasUpdated())
                    {
                        // If a mesh has been updated, force blending update
                        addTask([=]() { dynamic_pointer_cast<Blender>(_blender)->forceUpdate(); });
                        obj->setNotUpdated();
                    }
                if (objectCategory == GraphObject::Category::IMAGE || objectCategory == GraphObject::Category::TEXTURE)
                    if (obj->wasUpdated())
                        obj->setNotUpdated();

                obj->render();
            }

            if (objPriority.second.size() != 0)
                Timer::get() >> objPriority.second[0]->getType();

            if (firstWindowSync && objPriority.first >= GraphObject::Priority::POST_CAMERA)
            {
#ifdef PROFILE
                PROFILEGL("texture upload unlock");
#endif
                if (glIsSync(_cameraDrawnFence) == GL_TRUE)
                    glDeleteSync(_cameraDrawnFence);
                _cameraDrawnFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                if (textureLock.owns_lock())
                    textureLock.unlock();
                firstWindowSync = false;
            }
        }

        {
#ifdef PROFILE
            PROFILEGL("swap buffers");
#endif
            // Swap all buffers at once
            Timer::get() << "swap";
            for (auto& obj : _objects)
                if (obj.second->getType() == "window")
                    dynamic_pointer_cast<Window>(obj.second)->swapBuffers();
            Timer::get() >> "swap";
        }
    }

#ifdef PROFILE
    ProfilerGL::get().gatherTimings();
#endif
}

/*************/
void Scene::run()
{
    _textureUploadFuture = async(std::launch::async, [&]() { textureUploadRun(); });

    _mainWindow->setAsCurrentContext();
    while (_isRunning)
    {
        // This gets the whole loop duration
        if (_runInBackground && _swapInterval != 0)
        {
            // Artificial synchronization to avoid overloading the GPU in hidden mode
            Timer::get() >> _targetFrameDuration >> "swap_sync";
            Timer::get() << "swap_sync";
        }

        Timer::get() >> "loop_scene";
        Timer::get() << "loop_scene";

        // Execute waiting tasks
        runTasks();

        if (!_started)
        {
            this_thread::sleep_for(chrono::milliseconds(50));
            continue;
        }

        Timer::get() << "rendering";
        render();
        Timer::get() >> "rendering";

        Timer::get() << "inputsUpdate";
        updateInputs();
        Timer::get() >> "inputsUpdate";
    }
    _mainWindow->releaseContext();

    signalBufferObjectUpdated();
    _textureUploadFuture.wait();

#ifdef PROFILE
    ProfilerGL::get().processTimings();
    ProfilerGL::get().processFlamegraph("/tmp/splash_profiling_data_" + _name);
#endif
}

/*************/
void Scene::updateInputs()
{
    glfwPollEvents();

    auto keyboard = dynamic_pointer_cast<Keyboard>(_keyboard);
    if (keyboard)
        _gui->setKeyboardState(keyboard->getState(_name));

    auto mouse = dynamic_pointer_cast<Mouse>(_mouse);
    if (mouse)
        _gui->setMouseState(mouse->getState(_name));

    // Check if we should quit.
    if (Window::getQuitFlag())
        sendMessageToWorld("quit");
}

/*************/
void Scene::textureUploadRun()
{
    _textureUploadWindow->setAsCurrentContext();

    while (_isRunning)
    {
        if (!_started)
        {
            this_thread::sleep_for(chrono::milliseconds(50));
            continue;
        }

        waitSignalBufferObjectUpdated();
        Timer::get() >> "loop_texture";
        Timer::get() << "loop_texture";

        if (!_isRunning)
            break;

        {
#ifdef PROFILE
            PROFILEGL("Texture upload loop");
#endif

            unique_lock<Spinlock> lockTexture(_textureMutex);

            if (glIsSync(_cameraDrawnFence) == GL_TRUE)
            {
#ifdef PROFILE
                PROFILEGL("texture sync");
#endif
                glWaitSync(_cameraDrawnFence, 0, GL_TIMEOUT_IGNORED);
                glDeleteSync(_cameraDrawnFence);
            }

            Timer::get() << "textureUpload";

            vector<shared_ptr<Texture>> textures;
            bool expectedAtomicValue = false;
            if (_objectsCurrentlyUpdated.compare_exchange_strong(expectedAtomicValue, true, std::memory_order_acquire))
            {
                for (auto& obj : _objects)
                {
                    auto texture = dynamic_pointer_cast<Texture>(obj.second);
                    if (texture)
                        textures.emplace_back(texture);
                }
                _objectsCurrentlyUpdated.store(false, std::memory_order_release);
            }

            for (auto& texture : textures)
            {
#ifdef PROFILE
                PROFILEGL("start " + texture->getName());
#endif
                texture->update();
            }

            if (glIsSync(_textureUploadFence) == GL_TRUE)
                glDeleteSync(_textureUploadFence);
            _textureUploadFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            lockTexture.unlock();

            for (auto& texture : textures)
            {
#ifdef PROFILE
                PROFILEGL("end " + texture->getName());
#endif
                auto texImage = dynamic_pointer_cast<Texture_Image>(texture);
                if (texImage)
                    texImage->flushPbo();
            }

            Timer::get() >> "textureUpload";
        }

#ifdef PROFILE
        ProfilerGL::get().gatherTimings();
#endif
    }

    _textureUploadWindow->releaseContext();
}

/*************/
void Scene::setAsMaster(const string& configFilePath)
{
    lock_guard<recursive_mutex> lockObjects(_objectsMutex);

    _isMaster = true;

    _gui = make_shared<Gui>(_mainWindow, this);
    if (_gui)
    {
        _gui->setName("gui");
        _gui->setConfigFilePath(configFilePath);
        _objects["gui"] = _gui;
    }

    _keyboard = make_shared<Keyboard>(this);
    _mouse = make_shared<Mouse>(this);
    _joystick = make_shared<Joystick>(this);
    _dragndrop = make_shared<DragNDrop>(this);

    if (_keyboard)
    {
        _keyboard->setName("keyboard");
        _objects[_keyboard->getName()] = _keyboard;
    }
    if (_mouse)
    {
        _mouse->setName("keyboard");
        _objects["mouse"] = _mouse;
    }
    if (_joystick)
    {
        _joystick->setName("joystick");
        _objects[_joystick->getName()] = _joystick;
    }
    if (_dragndrop)
    {
        _dragndrop->setName("dragndrop");
        _objects[_dragndrop->getName()] = _dragndrop;
    }

#if HAVE_GPHOTO
    // Initialize the color calibration object
    _colorCalibrator = make_shared<ColorCalibrator>(this);
    _colorCalibrator->setName("colorCalibrator");
    _objects["colorCalibrator"] = _colorCalibrator;
#endif
}

/*************/
void Scene::sendMessageToWorld(const string& message, const Values& value)
{
    RootObject::sendMessage("world", message, value);
}

/*************/
Values Scene::sendMessageToWorldWithAnswer(const string& message, const Values& value, const unsigned long long timeout)
{
    return sendMessageWithAnswer("world", message, value, timeout);
}

/*************/
shared_ptr<GlWindow> Scene::getNewSharedWindow(const string& name)
{
    string windowName;
    name.size() == 0 ? windowName = "Splash::Window" : windowName = "Splash::" + name;

    if (!_mainWindow)
    {
        Log::get() << Log::WARNING << __FUNCTION__ << " - Main window does not exist, unable to create new shared window" << Log::endl;
        return {nullptr};
    }

    // The GL version is the same as in the initialization, so we don't have to reset it here
    glfwWindowHint(GLFW_SRGB_CAPABLE, GL_TRUE);
    glfwWindowHint(GLFW_VISIBLE, false);
    GLFWwindow* window = glfwCreateWindow(512, 512, windowName.c_str(), NULL, _mainWindow->get());
    if (!window)
    {
        Log::get() << Log::WARNING << __FUNCTION__ << " - Unable to create new shared window" << Log::endl;
        return {nullptr};
    }
    auto glWindow = make_shared<GlWindow>(window, _mainWindow->get());

    glWindow->setAsCurrentContext();
#if not HAVE_OSX
#ifdef DEBUGGL
    glDebugMessageCallback(Scene::glMsgCallback, (void*)this);
    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_MEDIUM, 0, nullptr, GL_TRUE);
    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_HIGH, 0, nullptr, GL_TRUE);
#endif
#endif

#if not HAVE_OSX
#ifdef GLX_NV_swap_group
    if (_maxSwapGroups)
    {
        PFNGLXJOINSWAPGROUPNVPROC nvGLJoinSwapGroup = (PFNGLXJOINSWAPGROUPNVPROC)glfwGetProcAddress("glXJoinSwapGroupNV");
        PFNGLXBINDSWAPBARRIERNVPROC nvGLBindSwapBarrier = (PFNGLXBINDSWAPBARRIERNVPROC)glfwGetProcAddress("glXBindSwapBarrierNV");

        bool nvResult = true;
        nvResult = nvGLJoinSwapGroup(glfwGetX11Display(), glfwGetGLXWindow(window), 1);
        if (nvResult)
            Log::get() << Log::MESSAGE << "Scene::" << __FUNCTION__ << " - Window " << windowName << " successfully joined the NV swap group" << Log::endl;
        else
            Log::get() << Log::MESSAGE << "Scene::" << __FUNCTION__ << " - Window " << windowName << " couldn't join the NV swap group" << Log::endl;

        nvResult = nvGLBindSwapBarrier(glfwGetX11Display(), 1, 1);
        if (nvResult)
            Log::get() << Log::MESSAGE << "Scene::" << __FUNCTION__ << " - Window " << windowName << " successfully bind the NV swap barrier" << Log::endl;
        else
            Log::get() << Log::MESSAGE << "Scene::" << __FUNCTION__ << " - Window " << windowName << " couldn't bind the NV swap barrier" << Log::endl;
    }
#endif
#endif
    glWindow->releaseContext();

    return glWindow;
}

/*************/
Values Scene::getObjectsNameByType(const string& type)
{
    lock_guard<recursive_mutex> lock(_objectsMutex);
    Values list;
    for (auto& obj : _objects)
        if (obj.second->getType() == type)
            list.push_back(obj.second->getName());
    return list;
}

/*************/
vector<int> Scene::findGLVersion()
{
    vector<vector<int>> glVersionList{{4, 5}};
    vector<int> detectedVersion{0, 0};

    for (auto version : glVersionList)
    {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, version[0]);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, version[1]);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if HAVE_OSX
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
        glfwWindowHint(GLFW_SRGB_CAPABLE, GL_TRUE);
        glfwWindowHint(GLFW_DEPTH_BITS, 24);
        glfwWindowHint(GLFW_VISIBLE, false);
        GLFWwindow* window = glfwCreateWindow(512, 512, "test_window", NULL, NULL);

        if (window)
        {
            detectedVersion = version;
            glfwDestroyWindow(window);
            break;
        }
    }

    return detectedVersion;
}

/*************/
void Scene::init(const string& name)
{
    glfwSetErrorCallback(Scene::glfwErrorCallback);

    // GLFW stuff
    if (!glfwInit())
    {
        Log::get() << Log::ERROR << "Scene::" << __FUNCTION__ << " - Unable to initialize GLFW" << Log::endl;
        _isInitialized = false;
        return;
    }

    auto glVersion = findGLVersion();
    if (glVersion[0] == 0)
    {
        Log::get() << Log::ERROR << "Scene::" << __FUNCTION__ << " - Unable to find a suitable GL version (higher than 4.3)" << Log::endl;
        _isInitialized = false;
        return;
    }

    _glVersion = glVersion;
    Log::get() << Log::MESSAGE << "Scene::" << __FUNCTION__ << " - GL version: " << glVersion[0] << "." << glVersion[1] << Log::endl;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, glVersion[0]);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, glVersion[1]);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef DEBUGGL
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, true);
#else
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, false);
#endif
    glfwWindowHint(GLFW_SRGB_CAPABLE, GL_TRUE);
    glfwWindowHint(GLFW_DEPTH_BITS, 24);
    glfwWindowHint(GLFW_VISIBLE, false);
#if HAVE_OSX
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(512, 512, name.c_str(), NULL, NULL);

    if (!window)
    {
        Log::get() << Log::WARNING << "Scene::" << __FUNCTION__ << " - Unable to create a GLFW window" << Log::endl;
        _isInitialized = false;
        return;
    }

    _mainWindow = make_shared<GlWindow>(window, window);
    _isInitialized = true;

    _mainWindow->setAsCurrentContext();
    gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);

// Activate GL debug messages
#if not HAVE_OSX
#ifdef DEBUGGL
    glDebugMessageCallback(Scene::glMsgCallback, (void*)this);
    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_MEDIUM, 0, nullptr, GL_TRUE);
    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_HIGH, 0, nullptr, GL_TRUE);
#endif
#endif

// Check for swap groups
#if not HAVE_OSX
#ifdef GLX_NV_swap_group
    if (glfwExtensionSupported("GLX_NV_swap_group"))
    {
        PFNGLXQUERYMAXSWAPGROUPSNVPROC nvGLQueryMaxSwapGroups = (PFNGLXQUERYMAXSWAPGROUPSNVPROC)glfwGetProcAddress("glXQueryMaxSwapGroupsNV");
        if (!nvGLQueryMaxSwapGroups(glfwGetX11Display(), 0, &_maxSwapGroups, &_maxSwapBarriers))
            Log::get() << Log::MESSAGE << "Scene::" << __FUNCTION__ << " - Unable to get NV max swap groups / barriers" << Log::endl;
        else
            Log::get() << Log::MESSAGE << "Scene::" << __FUNCTION__ << " - NV max swap groups: " << _maxSwapGroups << " / barriers: " << _maxSwapBarriers << Log::endl;

        if (_maxSwapGroups != 0)
            _hasNVSwapGroup = true;
    }
#endif
#endif
    _mainWindow->releaseContext();

    _textureUploadWindow = getNewSharedWindow();

    // Create the link and connect to the World
    _link = make_shared<Link>(this, name);
    _link->connectTo("world");
    sendMessageToWorld("sceneLaunched", {});
}

/*************/
unsigned long long Scene::updateTargetFrameDuration()
{
    auto mon = glfwGetPrimaryMonitor();
    if (!mon)
        return 0ull;

    auto vidMode = glfwGetVideoMode(mon);
    if (!vidMode)
        return 0ull;

    auto refreshRate = vidMode->refreshRate;
    if (!refreshRate)
        return 0ull;

    return static_cast<unsigned long long>(1e6 / refreshRate);
}

/*************/
void Scene::glfwErrorCallback(int /*code*/, const char* msg)
{
    Log::get() << Log::WARNING << "Scene::glfwErrorCallback - " << msg << Log::endl;
}

/*************/
#ifdef HAVE_OSX
void Scene::glMsgCallback(GLenum /*source*/, GLenum type, GLuint /*id*/, GLenum severity, GLsizei /*length*/, const GLchar* message, const void* /*userParam*/)
#else
void Scene::glMsgCallback(GLenum /*source*/, GLenum type, GLuint /*id*/, GLenum severity, GLsizei /*length*/, const GLchar* message, void* /*userParam*/)
#endif
{
    string typeString{"OTHER"};
    string severityString{""};
    Log::Priority logType{Log::MESSAGE};

    switch (type)
    {
    case GL_DEBUG_TYPE_ERROR:
        typeString = "Error";
        logType = Log::ERROR;
        break;
    case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
        typeString = "Deprecated behavior";
        logType = Log::WARNING;
        break;
    case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:
        typeString = "Undefined behavior";
        logType = Log::ERROR;
        break;
    case GL_DEBUG_TYPE_PORTABILITY:
        typeString = "Portability";
        logType = Log::WARNING;
        break;
    case GL_DEBUG_TYPE_PERFORMANCE:
        typeString = "Performance";
        logType = Log::WARNING;
        break;
    case GL_DEBUG_TYPE_OTHER:
        typeString = "Other";
        logType = Log::MESSAGE;
        break;
    }

    switch (severity)
    {
    case GL_DEBUG_SEVERITY_LOW:
        severityString = "low";
        break;
    case GL_DEBUG_SEVERITY_MEDIUM:
        severityString = "medium";
        break;
    case GL_DEBUG_SEVERITY_HIGH:
        severityString = "high";
        break;
    case GL_DEBUG_SEVERITY_NOTIFICATION:
        // Disable notifications, they are far too verbose
        return;
        // severityString = "notification";
        // break;
    }

    Log::get() << logType << "GL::debug - [" << typeString << "::" << severityString << "] - " << message << Log::endl;
}

/*************/
void Scene::registerAttributes()
{
    RootObject::registerAttributes();

    addAttribute("addObject",
        [&](const Values& args) {
            addTask([=]() {
                string type = args[0].as<string>();
                string name = args[1].as<string>();
                string sceneName = args.size() > 2 ? args[2].as<string>() : "";

                if (sceneName == _name)
                    addObject(type, name);
                else if (_isMaster)
                    addGhost(type, name);
            });

            return true;
        },
        {'s', 's'});
    setAttributeDescription("addObject", "Add an object of the given name, type, and optionally the target scene");

    addAttribute("config", [&](const Values&) {
        addTask([&]() -> void {
            setlocale(LC_NUMERIC, "C"); // Needed to make sure numbers are written with commas
            Json::Value config = getConfigurationAsJson();
            string configStr = config.toStyledString();
            sendMessageToWorld("answerMessage", {"config", _name, configStr});
        });
        return true;
    });
    setAttributeDescription("config", "Ask the Scene for a JSON describing its configuration");

    addAttribute("deleteObject",
        [&](const Values& args) {
            addTask([=]() -> void {
                // We wait until we can indeed deleted the object
                bool expectedAtomicValue = false;
                while (!_objectsCurrentlyUpdated.compare_exchange_strong(expectedAtomicValue, true, std::memory_order_acquire))
                    this_thread::sleep_for(chrono::milliseconds(1));
                OnScopeExit { _objectsCurrentlyUpdated.store(false, std::memory_order_release); };

                lock_guard<recursive_mutex> lockObjects(_objectsMutex);

                auto objectName = args[0].as<string>();
                auto object = getObject(objectName);
                if (!object)
                    return;

                for (auto& localObject : _objects)
                    unlink(object, localObject.second);
                _objects.erase(objectName);
            });

            return true;
        },
        {'s'});
    setAttributeDescription("deleteObject", "Delete an object given its name");

    addAttribute("duration",
        [&](const Values& args) {
            Timer::get().setDuration(args[0].as<string>(), args[1].as<int>());
            return true;
        },
        {'s', 'n'});
    setAttributeDescription("duration", "Set the duration of the given timer");

    addAttribute("masterClock",
        [&](const Values& args) {
            Timer::Point clock;
            clock.years = args[0].as<uint32_t>();
            clock.months = args[1].as<uint32_t>();
            clock.days = args[2].as<uint32_t>();
            clock.hours = args[3].as<uint32_t>();
            clock.mins = args[4].as<uint32_t>();
            clock.secs = args[5].as<uint32_t>();
            clock.frame = args[6].as<uint32_t>();
            clock.paused = args[7].as<bool>();
            Timer::get().setMasterClock(clock);
            return true;
        },
        {'n', 'n', 'n', 'n', 'n', 'n', 'n'});
    setAttributeDescription("masterClock", "Set the timing of the master clock");

    addAttribute("getObjectsNameByType",
        [&](const Values& args) {
            addTask([=]() {
                string type = args[0].as<string>();
                Values list = getObjectsNameByType(type);
                sendMessageToWorld("answerMessage", {"getObjectsNameByType", _name, list});
            });

            return true;
        },
        {'s'});
    setAttributeDescription("getObjectsNameByType", "Get a list of the objects having the given type");

    addAttribute("link",
        [&](const Values& args) {
            addTask([=]() {
                string src = args[0].as<string>();
                string dst = args[1].as<string>();
                link(src, dst);
            });

            return true;
        },
        {'s', 's'});
    setAttributeDescription("link", "Link the two given objects");

    addAttribute("log",
        [&](const Values& args) {
            Log::get().setLog(args[0].as<string>(), (Log::Priority)args[1].as<int>());
            return true;
        },
        {'s', 'n'});
    setAttributeDescription("log", "Add an entry to the logs, given its message and priority");

    addAttribute("logToFile",
        [&](const Values& args) {
            Log::get().logToFile(args[0].as<bool>());
            return true;
        },
        {'n'});
    setAttributeDescription("logToFile", "If set to 1, the process holding the Scene will try to write log to file");

    addAttribute("ping", [&](const Values&) {
        signalBufferObjectUpdated();
        sendMessageToWorld("pong", {_name});
        return true;
    });
    setAttributeDescription("ping", "Ping the World");

    addAttribute("sync", [&](const Values&) {
        addTask([=]() { sendMessageToWorld("answerMessage", {"sync", _name}); });
        return true;
    });
    setAttributeDescription("sync", "Dummy message to make sure all previous messages have been processed by the Scene.");

    addAttribute("remove",
        [&](const Values& args) {
            addTask([=]() {
                string name = args[1].as<string>();
                remove(name);
            });

            return true;
        },
        {'s'});
    setAttributeDescription("remove", "Remove the object of the given name");

    addAttribute("setAlias",
        [&](const Values& args) {
            auto name = args[0].as<string>();
            auto alias = args[1].as<string>();

            addTask([=]() {
                lock_guard<recursive_mutex> lock(_objectsMutex);

                auto object = getObject(name);
                if (object)
                    object->setAlias(alias);
            });

            return true;
        },
        {'s', 's'});

    addAttribute("setMaster", [&](const Values& args) {
        addTask([=]() {
            if (args.empty())
                setAsMaster();
            else
                setAsMaster(args[0].as<string>());
        });
        return true;
    });
    setAttributeDescription("setMaster", "Set this Scene as master, can give the configuration file path as a parameter");

    addAttribute("start", [&](const Values&) {
        _started = true;
        sendMessageToWorld("answerMessage", {"start", _name});
        return true;
    });
    setAttributeDescription("start", "Start the Scene main loop");

    addAttribute("stop", [&](const Values&) {
        _started = false;
        return true;
    });
    setAttributeDescription("stop", "Stop the Scene main loop");

    addAttribute("swapInterval",
        [&](const Values& args) {
            _swapInterval = max(-1, args[0].as<int>());
            _targetFrameDuration = updateTargetFrameDuration();
            return true;
        },
        [&]() -> Values { return {(int)_swapInterval}; },
        {'n'});
    setAttributeDescription("swapInterval", "Set the interval between two video frames. 1 is synced, 0 is not, -1 to sync when possible ");

    addAttribute("swapTest", [&](const Values& args) {
        addTask([=]() {
            lock_guard<recursive_mutex> lock(_objectsMutex);
            for (auto& obj : _objects)
                if (obj.second->getType() == "window")
                    dynamic_pointer_cast<Window>(obj.second)->setAttribute("swapTest", args);
        });
        return true;
    });
    setAttributeDescription("swapTest", "Activate video swap test if set to 1");

    addAttribute("swapTestColor", [&](const Values& args) {
        addTask([=]() {
            lock_guard<recursive_mutex> lock(_objectsMutex);
            for (auto& obj : _objects)
                if (obj.second->getType() == "window")
                    dynamic_pointer_cast<Window>(obj.second)->setAttribute("swapTestColor", args);
        });
        return true;
    });
    setAttributeDescription("swapTestColor", "Set the swap test color");

    addAttribute("quit", [&](const Values&) {
        addTask([=]() {
            _started = false;
            _isRunning = false;
        });
        return true;
    });
    setAttributeDescription("quit", "Ask the Scene to quit");

    addAttribute("unlink",
        [&](const Values& args) {
            addTask([=]() {
                string src = args[0].as<string>();
                string dst = args[1].as<string>();
                unlink(src, dst);
            });

            return true;
        },
        {'s', 's'});
    setAttributeDescription("unlink", "Unlink the two given objects");

    addAttribute("wireframe",
        [&](const Values& args) {
            addTask([=]() {
                lock_guard<recursive_mutex> lock(_objectsMutex);
                for (auto& obj : _objects)
                    if (obj.second->getType() == "camera")
                        dynamic_pointer_cast<Camera>(obj.second)->setAttribute("wireframe", {(int)(args[0].as<int>())});
            });

            return true;
        },
        {'n'});
    setAttributeDescription("wireframe", "Show all meshes as wireframes if set to 1");

#if HAVE_GPHOTO
    addAttribute("calibrateColor", [&](const Values&) {
        auto calibrator = dynamic_pointer_cast<ColorCalibrator>(_colorCalibrator);
        if (calibrator)
            calibrator->update();
        return true;
    });
    setAttributeDescription("calibrateColor", "Launch projectors color calibration");

    addAttribute("calibrateColorResponseFunction", [&](const Values&) {
        auto calibrator = dynamic_pointer_cast<ColorCalibrator>(_colorCalibrator);
        if (calibrator)
            calibrator->updateCRF();
        return true;
    });
    setAttributeDescription("calibrateColorResponseFunction", "Launch the camera color calibration");
#endif

    addAttribute("configurationPath",
        [&](const Values& args) {
            _configurationPath = args[0].as<string>();
            return true;
        },
        {'s'});
    setAttributeDescription("configurationPath", "Path to the configuration files");

    addAttribute("mediaPath",
        [&](const Values& args) {
            _mediaPath = args[0].as<string>();
            return true;
        },
        {'s'});
    setAttributeDescription("mediaPath", "Path to the media files");

    addAttribute("runInBackground",
        [&](const Values& args) {
            _runInBackground = args[0].as<bool>();
            return true;
        },
        {'n'});
    setAttributeDescription("runInBackground", "If set to 1, Splash will run in the background (useful for background processing)");
}

} // namespace Splash
