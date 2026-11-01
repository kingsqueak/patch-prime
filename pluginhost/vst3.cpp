// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

#include "vst3.h"

#include "x11window.h"

#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/smartpointer.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <QByteArray>
#include <QObject>
#include <QSocketNotifier>
#include <QString>
#include <QTimer>

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <print>
#include <set>
#include <span>
#include <thread>

using namespace Steinberg; // NOLINT(google-build-using-namespace): the VST3 API lives there

// The VST3 API's own idioms, used as they are: IPtr tests as a pointer, ids
// and names are char arrays, TBool is an unsigned char.
// NOLINTBEGIN(readability-implicit-bool-conversion,cppcoreguidelines-pro-bounds-array-to-pointer-decay)

namespace pp {

namespace {

// Stepped parameters with up to this many steps are choices (or switches).
constexpr int32 maxChoiceSteps = 31;

template<typename I>
IPtr<I> query(FUnknown *object)
{
    I *out = nullptr;
    if (object != nullptr &&
        object->queryInterface(I::iid, reinterpret_cast<void **>(&out)) == kResultOk) { // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        return owned(out);
    }
    return nullptr;
}

// A String128 (or other fixed UTF-16 field), which a plugin may fill
// without an end mark.
std::string utf8(const Vst::TChar *text, std::size_t size = 128)
{
    std::size_t n = 0;
    while (n < size && text[n] != 0) { // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        ++n;
    }
    return QString::fromUtf16(text, static_cast<qsizetype>(n)).toStdString();
}

// A fixed char field the same way.
std::string text(const char8 *field, std::size_t size)
{
    return {field, strnlen(field, size)};
}

void copyUtf16(Vst::String128 to, const char16_t *from)
{
    std::size_t i = 0;
    for (; from[i] != 0 && i + 1 < 128; ++i) { // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        to[i] = from[i];                         // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    }
    to[i] = 0; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
}

// A bundle's library: Foo.vst3/Contents/<arch>-linux/Foo.so (or the bundle
// itself, when it's a plain file).
std::string libraryOf(const std::string &bundle)
{
    const std::filesystem::path path(bundle);
    if (std::filesystem::is_regular_file(path)) {
        return bundle;
    }
#ifdef __aarch64__
    constexpr const char *arch = "aarch64-linux";
#else
    constexpr const char *arch = "x86_64-linux";
#endif
    return (path / "Contents" / arch / (path.stem().string() + ".so")).string();
}

// A bundle's factory, loaded once and kept for the process's life.
IPtr<IPluginFactory> loadFactory(const std::string &bundle, std::string &error)
{
    static std::map<std::string, IPtr<IPluginFactory>> loaded;
    if (const auto it = loaded.find(bundle); it != loaded.end()) {
        return it->second;
    }
    void *library = dlopen(libraryOf(bundle).c_str(), RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        error = std::string("it couldn't be loaded: ") + dlerror();
        return nullptr;
    }
    using Entry = bool (*)(void *);
    using GetFactory = IPluginFactory *(*)();
    if (auto *entry = reinterpret_cast<Entry>(dlsym(library, "ModuleEntry")); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        entry != nullptr && !entry(library)) {
        error = "its library didn't start";
        return nullptr;
    }
    auto *getFactory = reinterpret_cast<GetFactory>(dlsym(library, "GetPluginFactory")); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    if (getFactory == nullptr) {
        error = "it isn't a VST3 plugin";
        return nullptr;
    }
    IPtr<IPluginFactory> factory = owned(getFactory());
    if (!factory) {
        error = "it has no plugins";
        return nullptr;
    }
    loaded[bundle] = factory;
    return factory;
}

// --- What a host gives a plugin -------------------------------------------------

// A stream in memory (states).
class MemoryStream : public U::ImplementsNonDestroyable<U::Directly<IBStream>>
{
public:
    // A state bigger than this is refused (a plugin gone wrong).
    static constexpr int64 maxSize = int64{256} << 20;
    QByteArray data;
    int64 at = 0;

    tresult PLUGIN_API read(void *buffer, int32 numBytes, int32 *numBytesRead) override
    {
        const auto n = std::clamp<int64>(numBytes, 0, std::max<int64>(data.size() - at, 0));
        std::memcpy(buffer, data.constData() + at, static_cast<std::size_t>(n)); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        at += n;
        if (numBytesRead != nullptr) {
            *numBytesRead = static_cast<int32>(n);
        }
        return kResultOk;
    }
    tresult PLUGIN_API write(void *buffer, int32 numBytes, int32 *numBytesWritten) override
    {
        if (numBytes < 0) {
            return kInvalidArgument;
        }
        const auto end = at + numBytes;
        if (end > maxSize) {
            return kOutOfMemory;
        }
        if (end > data.size()) {
            data.resize(static_cast<qsizetype>(end), '\0'); // a gap left by seeking is zeros
        }
        std::memcpy(data.data() + at, buffer, static_cast<std::size_t>(numBytes)); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        at = end;
        if (numBytesWritten != nullptr) {
            *numBytesWritten = numBytes;
        }
        return kResultOk;
    }
    tresult PLUGIN_API seek(int64 pos, int32 mode, int64 *result) override
    {
        const int64 base = mode == kIBSeekCur ? at : mode == kIBSeekEnd ? data.size() : 0; // NOLINT(readability-avoid-nested-conditional-operator)
        // Past the end is allowed (a writer reserving room for a header);
        // writing there fills the gap.
        at = std::clamp<int64>(base + pos, 0, maxSize);
        if (result != nullptr) {
            *result = at;
        }
        return kResultOk;
    }
    tresult PLUGIN_API tell(int64 *pos) override
    {
        if (pos != nullptr) {
            *pos = at;
        }
        return kResultOk;
    }
};

// Attributes of a message between a plugin's component and its controller.
class AttributeList : public U::Implements<U::Directly<Vst::IAttributeList>>
{
public:
    tresult PLUGIN_API setInt(AttrID id, int64 value) override
    {
        m_ints[id] = value;
        return kResultOk;
    }
    tresult PLUGIN_API getInt(AttrID id, int64 &value) override
    {
        const auto it = m_ints.find(id);
        if (it == m_ints.end()) {
            return kResultFalse;
        }
        value = it->second;
        return kResultOk;
    }
    tresult PLUGIN_API setFloat(AttrID id, double value) override
    {
        m_floats[id] = value;
        return kResultOk;
    }
    tresult PLUGIN_API getFloat(AttrID id, double &value) override
    {
        const auto it = m_floats.find(id);
        if (it == m_floats.end()) {
            return kResultFalse;
        }
        value = it->second;
        return kResultOk;
    }
    tresult PLUGIN_API setString(AttrID id, const Vst::TChar *string) override
    {
        m_strings[id] = std::u16string(string);
        return kResultOk;
    }
    tresult PLUGIN_API getString(AttrID id, Vst::TChar *string, uint32 sizeInBytes) override
    {
        const auto it = m_strings.find(id);
        if (it == m_strings.end() || sizeInBytes < sizeof(Vst::TChar)) {
            return kResultFalse;
        }
        const auto n = std::min<std::size_t>(it->second.size(), (sizeInBytes / sizeof(Vst::TChar)) - 1);
        std::memcpy(string, it->second.data(), n * sizeof(Vst::TChar));
        string[n] = 0; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        return kResultOk;
    }
    tresult PLUGIN_API setBinary(AttrID id, const void *data, uint32 sizeInBytes) override
    {
        const auto *bytes = static_cast<const char *>(data);
        m_binaries[id] = std::vector<char>(bytes, bytes + sizeInBytes); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        return kResultOk;
    }
    tresult PLUGIN_API getBinary(AttrID id, const void *&data, uint32 &sizeInBytes) override
    {
        const auto it = m_binaries.find(id);
        if (it == m_binaries.end()) {
            return kResultFalse;
        }
        data = it->second.data();
        sizeInBytes = static_cast<uint32>(it->second.size());
        return kResultOk;
    }

private:
    std::map<std::string, int64> m_ints;
    std::map<std::string, double> m_floats;
    std::map<std::string, std::u16string> m_strings;
    std::map<std::string, std::vector<char>> m_binaries;
};

class Message : public U::Implements<U::Directly<Vst::IMessage>>
{
public:
    FIDString PLUGIN_API getMessageID() override { return m_id.c_str(); }
    void PLUGIN_API setMessageID(FIDString id) override { m_id = id != nullptr ? id : ""; }
    Vst::IAttributeList *PLUGIN_API getAttributes() override { return m_attributes.get(); }

private:
    std::string m_id;
    IPtr<AttributeList> m_attributes = owned(new AttributeList);
};

// The host context: also offers the Linux run loop (a plugin may look for it
// here, before it has an editor), through the frame that implements it.
class HostApplication : public U::ImplementsNonDestroyable<U::Directly<Vst::IHostApplication, Linux::IRunLoop>>
{
public:
    Linux::IRunLoop *runLoop = nullptr;

    tresult PLUGIN_API registerEventHandler(Linux::IEventHandler *handler, Linux::FileDescriptor fd) override
    {
        return runLoop != nullptr ? runLoop->registerEventHandler(handler, fd) : kResultFalse;
    }
    tresult PLUGIN_API unregisterEventHandler(Linux::IEventHandler *handler) override
    {
        return runLoop != nullptr ? runLoop->unregisterEventHandler(handler) : kResultFalse;
    }
    tresult PLUGIN_API registerTimer(Linux::ITimerHandler *handler, Linux::TimerInterval milliseconds) override
    {
        return runLoop != nullptr ? runLoop->registerTimer(handler, milliseconds) : kResultFalse;
    }
    tresult PLUGIN_API unregisterTimer(Linux::ITimerHandler *handler) override
    {
        return runLoop != nullptr ? runLoop->unregisterTimer(handler) : kResultFalse;
    }

    tresult PLUGIN_API getName(Vst::String128 name) override
    {
        copyUtf16(name, u"Patch Prime");
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(TUID cid, TUID interfaceTuid, void **obj) override
    {
        const FUID classId = FUID::fromTUID(cid);
        const FUID interfaceId = FUID::fromTUID(interfaceTuid);
        if (classId == Vst::IMessage::iid && interfaceId == Vst::IMessage::iid) {
            *obj = static_cast<Vst::IMessage *>(new Message);
            return kResultOk;
        }
        if (classId == Vst::IAttributeList::iid && interfaceId == Vst::IAttributeList::iid) {
            *obj = static_cast<Vst::IAttributeList *>(new AttributeList);
            return kResultOk;
        }
        *obj = nullptr;
        return kResultFalse;
    }
};

// One parameter's changes in a block: a single point (its value at the start).
class ParamQueue : public U::ImplementsNonDestroyable<U::Directly<Vst::IParamValueQueue>>
{
public:
    Vst::ParamID id = 0;
    Vst::ParamValue value = 0.0;
    int32 points = 0;

    Vst::ParamID PLUGIN_API getParameterId() override { return id; }
    int32 PLUGIN_API getPointCount() override { return points; }
    tresult PLUGIN_API getPoint(int32 index, int32 &sampleOffset, Vst::ParamValue &v) override
    {
        if (index < 0 || index >= points) {
            return kResultFalse;
        }
        sampleOffset = 0;
        v = value;
        return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32 /*sampleOffset*/, Vst::ParamValue v, int32 &index) override
    {
        value = v; // the last one is what counts here
        points = 1;
        index = 0;
        return kResultOk;
    }
};

// A block's parameter changes, in queues made beforehand (no allocation on
// the realtime thread).
class ParamChanges : public U::ImplementsNonDestroyable<U::Directly<Vst::IParameterChanges>>
{
public:
    std::vector<ParamQueue> queues;
    int32 count = 0;

    int32 PLUGIN_API getParameterCount() override { return count; }
    Vst::IParamValueQueue *PLUGIN_API getParameterData(int32 index) override
    {
        return index >= 0 && index < count ? &queues[static_cast<std::size_t>(index)] : nullptr;
    }
    Vst::IParamValueQueue *PLUGIN_API addParameterData(const Vst::ParamID &id, int32 &index) override
    {
        for (int32 i = 0; i < count; ++i) {
            if (queues[static_cast<std::size_t>(i)].id == id) {
                index = i;
                return &queues[static_cast<std::size_t>(i)];
            }
        }
        if (std::cmp_greater_equal(count, queues.size())) {
            return nullptr;
        }
        auto &queue = queues[static_cast<std::size_t>(count)];
        queue.id = id;
        queue.points = 0;
        index = count++;
        return &queue;
    }
};

class EventList : public U::ImplementsNonDestroyable<U::Directly<Vst::IEventList>>
{
public:
    int32 PLUGIN_API getEventCount() override { return 0; }
    tresult PLUGIN_API getEvent(int32 /*index*/, Vst::Event & /*e*/) override { return kResultFalse; }
    tresult PLUGIN_API addEvent(Vst::Event & /*e*/) override { return kResultOk; }
};

} // namespace

// --- The plugin ---------------------------------------------------------------

class Vst3Window;

struct Vst3Effect::Impl
{
    struct Param
    {
        Vst::ParamID id = 0;
        int32 steps = 0; // a choice or switch with this many steps, or 0
        double initial = 0.0; // normalized
        bool settable = true;
    };

    // The editor's edits, restarts: the controller calls back here.
    class Handler : public U::ImplementsNonDestroyable<U::Directly<Vst::IComponentHandler>>
    {
    public:
        explicit Handler(Impl &impl) : m_impl(&impl) {}
        tresult PLUGIN_API beginEdit(Vst::ParamID /*id*/) override { return kResultOk; }
        tresult PLUGIN_API performEdit(Vst::ParamID id, Vst::ParamValue value) override
        {
            m_impl->edited(id, value);
            return kResultOk;
        }
        tresult PLUGIN_API endEdit(Vst::ParamID /*id*/) override { return kResultOk; }
        tresult PLUGIN_API restartComponent(int32 flags) override
        {
            m_impl->restart(flags);
            return kResultOk;
        }

    private:
        Impl *m_impl;
    };

    // The editor's frame: resizing, and the run loop (timers and file
    // descriptors on the main thread, through Qt).
    class Frame : public U::ImplementsNonDestroyable<U::Directly<IPlugFrame, Linux::IRunLoop>>
    {
    public:
        explicit Frame(Impl &impl) : m_impl(&impl) {}
        tresult PLUGIN_API resizeView(IPlugView *view, ViewRect *newSize) override;
        tresult PLUGIN_API registerEventHandler(Linux::IEventHandler *eventHandler, Linux::FileDescriptor fd) override
        {
            auto *notifier = new QSocketNotifier(fd, QSocketNotifier::Read, m_impl->context);
            QObject::connect(notifier, &QSocketNotifier::activated, m_impl->context,
                             [eventHandler, fd] { eventHandler->onFDIsSet(fd); });
            m_impl->fds.emplace(eventHandler, notifier);
            return kResultOk;
        }
        tresult PLUGIN_API unregisterEventHandler(Linux::IEventHandler *eventHandler) override
        {
            const auto [first, last] = m_impl->fds.equal_range(eventHandler);
            for (auto it = first; it != last; ++it) {
                it->second->setEnabled(false);
                it->second->deleteLater();
            }
            m_impl->fds.erase(eventHandler);
            return kResultOk;
        }
        tresult PLUGIN_API registerTimer(Linux::ITimerHandler *timerHandler, Linux::TimerInterval milliseconds) override
        {
            auto *timer = new QTimer(m_impl->context);
            QObject::connect(timer, &QTimer::timeout, m_impl->context, [timerHandler] { timerHandler->onTimer(); });
            timer->start(static_cast<int>(std::max<Linux::TimerInterval>(milliseconds, 1)));
            m_impl->timers.emplace(timerHandler, timer);
            return kResultOk;
        }
        tresult PLUGIN_API unregisterTimer(Linux::ITimerHandler *timerHandler) override
        {
            const auto [first, last] = m_impl->timers.equal_range(timerHandler);
            for (auto it = first; it != last; ++it) {
                it->second->stop();
                it->second->deleteLater();
            }
            m_impl->timers.erase(timerHandler);
            return kResultOk;
        }

    private:
        Impl *m_impl;
    };

    Impl() : handler(*this), frame(*this), context(new QObject) { host.runLoop = &frame; }
    ~Impl()
    {
        destroy();
        delete context;
    }
    Impl(const Impl &) = delete;
    Impl &operator=(const Impl &) = delete;
    Impl(Impl &&) = delete;
    Impl &operator=(Impl &&) = delete;

    bool create(const IPtr<IPluginFactory> &factory, const TUID cid);
    void readParams();
    void destroy();
    void edited(Vst::ParamID id, double value);
    void restart(int32 flags);
    void takeOverValues();
    void restoreState() const;
    [[nodiscard]] std::size_t indexOf(Vst::ParamID id) const
    {
        const auto it = paramIndex.find(id);
        return it == paramIndex.end() ? params.size() : it->second;
    }
    // Ours (a step's index, or normalized) <-> VST3's (normalized).
    [[nodiscard]] double toOurs(std::size_t i, double normalized) const
    {
        return params[i].steps > 0 ? std::round(normalized * params[i].steps) : normalized;
    }

    std::string type;
    std::string error;
    std::string initialState;
    HostApplication host;
    Handler handler;
    Frame frame;
    IPtr<Vst::IComponent> component;
    IPtr<Vst::IAudioProcessor> processor;
    IPtr<Vst::IEditController> controller;
    IPtr<Vst::IConnectionPoint> componentLink;
    IPtr<Vst::IConnectionPoint> controllerLink;
    bool separateController = false;
    bool active = false;
    bool processing = false;
    // A restart the plugin asked for (restartComponent's flags), done on the
    // main thread's tick; the audio thread is kept out while it happens.
    std::atomic<int32> pendingRestart{0};
    std::atomic<bool> suspended{false};
    std::atomic<bool> inProcess{false};
    bool reload = false;
    double lastRate = 48000.0;
    int lastChannels = 2;

    std::vector<Param> params;
    std::map<Vst::ParamID, std::size_t> paramIndex;
    std::unique_ptr<std::atomic<double>[]> targets; // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays): normalized, shared with the realtime thread
    std::unique_ptr<std::atomic<bool>[]> changed;   // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays): by the plugin, for the engine
    std::vector<double> controllerSent; // main thread: what the controller was last told
    std::vector<double> block;          // realtime: this block's values
    std::vector<double> sent;           // realtime: what the processor was last sent

    ParamChanges in;
    ParamChanges out;
    EventList events;
    Vst::ProcessContext processContext{};
    std::vector<int32> inChannels;
    std::vector<int32> outChannels;
    int mainIn = -1;
    int mainOut = -1;
    std::vector<std::vector<std::vector<float>>> inBuffers;
    std::vector<std::vector<std::vector<float>>> outBuffers;
    std::vector<std::vector<float *>> inPointers;
    std::vector<std::vector<float *>> outPointers;
    std::vector<Vst::AudioBusBuffers> inBuses;
    std::vector<Vst::AudioBusBuffers> outBuses;
    std::vector<float> silence;
    int64 time = 0;

    Vst3Window *window = nullptr;
    QObject *context; // owns the run loop's timers and notifiers
    std::multimap<Linux::IEventHandler *, QSocketNotifier *> fds;
    std::multimap<Linux::ITimerHandler *, QTimer *> timers;
};

// The plugin's own editor (IPlugView, X11) in an X11Window.
class Vst3Window : public PluginWindow
{
public:
    Vst3Window(Vst3Effect::Impl &impl, const std::string &title) : m_impl(&impl)
    {
        if (!impl.controller || !impl.active) {
            m_error = "the plugin isn't running";
            return;
        }
        m_view = owned(impl.controller->createView(Vst::ViewType::kEditor));
        if (!m_view || m_view->isPlatformTypeSupported(kPlatformTypeX11EmbedWindowID) != kResultTrue) {
            m_view = nullptr;
            m_error = "this plugin has no window Patch Prime can show";
            return;
        }
        ViewRect size{};
        const bool sized = m_view->getSize(&size) == kResultOk && size.getWidth() > 0 &&
                           size.getHeight() > 0;
        if (!m_window.create(title, sized ? size.getWidth() : 640, sized ? size.getHeight() : 480)) {
            m_error = "there's no display to show it on";
            return;
        }
        impl.window = this;
        m_window.setEmbedder(true); // VST3's X11 editors are XEmbed clients
        m_view->setFrame(&impl.frame);
        if (m_view->attached(reinterpret_cast<void *>(m_window.id()), kPlatformTypeX11EmbedWindowID) != kResultOk) { // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr): the XID as VST3 passes it
            m_error = "its window couldn't be embedded";
            return;
        }
        m_attached = true;
        if (m_view->getSize(&size) == kResultOk && size.getWidth() > 0) {
            m_window.resize(size.getWidth(), size.getHeight());
        }
        // kResultTrue: VST3's "yes" (an editor that answers anything else
        // keeps its size, or changes it itself through resizeView).
        m_window.setResizable(m_view->canResize() == kResultTrue);
        m_window.show();
    }
    ~Vst3Window() override
    {
        m_impl->window = nullptr;
        if (m_view) {
            if (m_attached) {
                m_view->removed();
            }
            m_view->setFrame(nullptr);
        }
        m_view = nullptr;
        m_window.destroy();
    }
    Vst3Window(const Vst3Window &) = delete;
    Vst3Window &operator=(const Vst3Window &) = delete;
    Vst3Window(Vst3Window &&) = delete;
    Vst3Window &operator=(Vst3Window &&) = delete;

    [[nodiscard]] const std::string &error() const override { return m_error; }
    bool idle() override
    {
        if (!m_attached || !m_window.isOpen()) {
            return false;
        }
        const bool open = m_window.pump([this](int width, int height) {
            // The user resized the window: the editor follows if it can, else
            // the window goes back to the editor's size.
            // In VST3's order: the size it accepts, the window to that size,
            // then the editor told.
            ViewRect rect(0, 0, width, height);
            if (m_view->canResize() == kResultTrue) {
                m_view->checkSizeConstraint(&rect);
                if (rect.getWidth() != width || rect.getHeight() != height) {
                    m_window.resize(rect.getWidth(), rect.getHeight());
                }
                resizing(rect);
            } else {
                m_view->getSize(&rect);
                m_window.resize(rect.getWidth(), rect.getHeight());
            }
        });
        m_window.flush();
        return open;
    }
    void raise() override { m_window.raise(); }

    // The editor asked for another size.
    void resizeTo(ViewRect &rect)
    {
        if (m_inResize) {
            return; // asked again from inside onSize: it has that size already
        }
        m_window.resize(rect.getWidth(), rect.getHeight());
        resizing(rect);
    }

private:
    void resizing(ViewRect &rect)
    {
        m_inResize = true;
        m_view->onSize(&rect);
        m_inResize = false;
    }

public:

private:
    Vst3Effect::Impl *m_impl;
    std::string m_error;
    X11Window m_window;
    IPtr<IPlugView> m_view;
    bool m_attached = false;
    bool m_inResize = false;
};

tresult PLUGIN_API Vst3Effect::Impl::Frame::resizeView(IPlugView * /*view*/, ViewRect *newSize)
{
    if (m_impl->window == nullptr || newSize == nullptr) {
        return kResultFalse;
    }
    m_impl->window->resizeTo(*newSize);
    return kResultTrue;
}

bool Vst3Effect::Impl::create(const IPtr<IPluginFactory> &factory, const TUID cid)
{
    Vst::IComponent *raw = nullptr;
    if (factory->createInstance(cid, Vst::IComponent::iid, reinterpret_cast<void **>(&raw)) != kResultOk || // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        raw == nullptr) {
        error = "the plugin couldn't be created";
        return false;
    }
    component = owned(raw);
    if (component->initialize(static_cast<Vst::IHostApplication *>(&host)) != kResultOk) {
        error = "the plugin couldn't be started";
        component = nullptr;
        return false;
    }
    processor = query<Vst::IAudioProcessor>(component);
    if (!processor) {
        error = "it doesn't process audio";
        return false;
    }
    // Its controller: the component itself, or an object of its own.
    controller = query<Vst::IEditController>(component);
    TUID controllerId{};
    if (!controller && component->getControllerClassId(controllerId) == kResultOk) {
        Vst::IEditController *rawController = nullptr;
        if (factory->createInstance(controllerId, Vst::IEditController::iid,
                                    reinterpret_cast<void **>(&rawController)) == kResultOk && // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
            rawController != nullptr) {
            controller = owned(rawController);
            if (controller->initialize(static_cast<Vst::IHostApplication *>(&host)) != kResultOk) {
                controller = nullptr;
            } else {
                separateController = true;
            }
        }
    }
    if (controller) {
        if (separateController) {
            componentLink = query<Vst::IConnectionPoint>(component);
            controllerLink = query<Vst::IConnectionPoint>(controller);
            if (componentLink && controllerLink) {
                componentLink->connect(controllerLink);
                controllerLink->connect(componentLink);
            }
        }
        controller->setComponentHandler(&handler);
        // The controller starts from the component's state, as hosts do.
        MemoryStream state;
        if (component->getState(&state) == kResultOk && !state.data.isEmpty()) {
            state.at = 0;
            controller->setComponentState(&state);
        }
    }
    readParams();
    return true;
}

void Vst3Effect::Impl::readParams()
{
    params.clear();
    paramIndex.clear();
    if (controller) {
        for (int32 i = 0; i < controller->getParameterCount(); ++i) {
            Vst::ParameterInfo info{};
            if (controller->getParameterInfo(i, info) != kResultOk) {
                continue;
            }
            const auto flags = info.flags;
            paramIndex[info.id] = params.size();
            params.push_back({.id = info.id,
                              .steps = info.stepCount > 0 && info.stepCount <= maxChoiceSteps ? info.stepCount : 0,
                              .initial = controller->getParamNormalized(info.id),
                              // A program change is a parameter like any other
                              // to the processor.
                              .settable = (flags & (Vst::ParameterInfo::kIsReadOnly |
                                                    Vst::ParameterInfo::kIsHidden)) == 0});
        }
    }
    targets = std::make_unique<std::atomic<double>[]>(params.size()); // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
    changed = std::make_unique<std::atomic<bool>[]>(params.size());   // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
    for (std::size_t i = 0; i < params.size(); ++i) {
        targets[i].store(params[i].initial);
        changed[i].store(false);
    }
    controllerSent.assign(params.size(), std::numeric_limits<double>::quiet_NaN());
}

void Vst3Effect::Impl::destroy()
{
    if (processing && processor) {
        processor->setProcessing(false);
    }
    processing = false;
    if (active && component) {
        component->setActive(false);
    }
    active = false;
    if (componentLink && controllerLink) {
        componentLink->disconnect(controllerLink);
        controllerLink->disconnect(componentLink);
    }
    componentLink = nullptr;
    controllerLink = nullptr;
    if (controller) {
        controller->setComponentHandler(nullptr);
        if (separateController) {
            controller->terminate();
        }
    }
    controller = nullptr;
    processor = nullptr;
    if (component) {
        component->terminate();
    }
    component = nullptr;
}

void Vst3Effect::Impl::edited(Vst::ParamID id, double value)
{
    const auto i = indexOf(id);
    if (i == params.size()) {
        return;
    }
    targets[i].store(value);
    controllerSent[i] = value; // it knows
    changed[i].store(true);
}

void Vst3Effect::Impl::restart(int32 flags)
{
    // Noted, and done on the main thread's next tick (not inside the
    // plugin's own call).
    pendingRestart.fetch_or(flags);
}

void Vst3Effect::Impl::takeOverValues()
{
    if (!controller) {
        return;
    }
    // The controller changed its values (a preset): take them over.
    for (std::size_t i = 0; i < params.size(); ++i) {
        const double value = controller->getParamNormalized(params[i].id);
        if (value != targets[i].load()) {
            edited(params[i].id, value);
        }
    }
}

void Vst3Effect::Impl::restoreState() const
{
    if (initialState.empty()) {
        return;
    }
    // [length][component state][length][controller state]
    const QByteArray data = QByteArray::fromBase64(QByteArray::fromStdString(initialState));
    qsizetype at = 0;
    const auto part = [&]() -> QByteArray {
        int32 length = 0;
        if (data.size() - at < static_cast<qsizetype>(sizeof(length))) {
            return {};
        }
        std::memcpy(&length, data.constData() + at, sizeof(length)); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        at += static_cast<qsizetype>(sizeof(length));
        length = std::clamp<int32>(length, 0, static_cast<int32>(data.size() - at));
        QByteArray bytes = data.mid(at, length);
        at += length;
        return bytes;
    };
    MemoryStream componentState;
    componentState.data = part();
    MemoryStream controllerState;
    controllerState.data = part();
    if (!componentState.data.isEmpty()) {
        component->setState(&componentState);
        if (controller) {
            componentState.at = 0;
            controller->setComponentState(&componentState);
        }
    }
    if (controller && separateController && !controllerState.data.isEmpty()) {
        controller->setState(&controllerState);
    }
}

// --- Vst3Effect ---------------------------------------------------------------

Vst3Effect::Vst3Effect(const std::string &path, const std::string &classId) : d(std::make_unique<Impl>())
{
    d->type = std::string(vst3TypePrefix) + classId;
    const auto factory = loadFactory(path, d->error);
    if (!factory) {
        return;
    }
    // The factory gets the host context too (some plugins set up their run
    // loop there).
    if (const auto factory3 = query<IPluginFactory3>(factory)) {
        factory3->setHostContext(static_cast<Vst::IHostApplication *>(&d->host));
    }
    FUID uid;
    if (!uid.fromString(classId.c_str())) {
        d->error = "the plugin's id isn't valid";
        return;
    }
    d->create(factory, uid.toTUID());
}

Vst3Effect::~Vst3Effect() = default;

std::string_view Vst3Effect::type() const
{
    return d->type;
}

const std::string &Vst3Effect::error() const
{
    return d->error;
}

void Vst3Effect::prepare(double sampleRate, int channels)
{
    Impl &p = *d;
    if (!p.error.empty() || !p.component) {
        return;
    }
    // Buses: the main ones carry the strip (mono or stereo, if the plugin
    // takes that); others (sidechains) are off.
    p.lastRate = sampleRate;
    p.lastChannels = channels;
    p.mainIn = -1;
    p.mainOut = -1;
    const int32 ins = std::clamp(p.component->getBusCount(Vst::kAudio, Vst::kInput), 0, 64);
    const int32 outs = std::clamp(p.component->getBusCount(Vst::kAudio, Vst::kOutput), 0, 64);
    std::vector<Vst::SpeakerArrangement> inArr(static_cast<std::size_t>(ins), Vst::SpeakerArr::kStereo);
    std::vector<Vst::SpeakerArrangement> outArr(static_cast<std::size_t>(outs), Vst::SpeakerArr::kStereo);
    for (const bool input : {true, false}) {
        const auto dir = input ? Vst::kInput : Vst::kOutput;
        auto &arr = input ? inArr : outArr;
        int &main = input ? p.mainIn : p.mainOut;
        for (int32 i = 0; i < (input ? ins : outs); ++i) {
            p.processor->getBusArrangement(dir, i, arr[static_cast<std::size_t>(i)]);
            Vst::BusInfo info{};
            if (main < 0 && p.component->getBusInfo(Vst::kAudio, dir, i, info) == kResultOk &&
                info.busType == Vst::kMain) {
                main = i;
            }
        }
    }
    if (p.mainIn < 0 || p.mainOut < 0) {
        p.error = "it has no main audio input and output";
        return;
    }
    const auto wanted = channels == 1 ? Vst::SpeakerArr::kMono : Vst::SpeakerArr::kStereo;
    inArr[static_cast<std::size_t>(p.mainIn)] = wanted;
    outArr[static_cast<std::size_t>(p.mainOut)] = wanted;
    p.processor->setBusArrangements(inArr.data(), ins, outArr.data(), outs);
    // What it settled on (it may refuse mono, say).
    p.inChannels.assign(static_cast<std::size_t>(ins), 0);
    p.outChannels.assign(static_cast<std::size_t>(outs), 0);
    for (int32 i = 0; i < ins; ++i) {
        Vst::SpeakerArrangement arr = 0;
        p.processor->getBusArrangement(Vst::kInput, i, arr);
        p.inChannels[static_cast<std::size_t>(i)] = Vst::SpeakerArr::getChannelCount(arr);
        p.component->activateBus(Vst::kAudio, Vst::kInput, i, i == p.mainIn);
    }
    for (int32 i = 0; i < outs; ++i) {
        Vst::SpeakerArrangement arr = 0;
        p.processor->getBusArrangement(Vst::kOutput, i, arr);
        p.outChannels[static_cast<std::size_t>(i)] = Vst::SpeakerArr::getChannelCount(arr);
        p.component->activateBus(Vst::kAudio, Vst::kOutput, i, i == p.mainOut);
    }
    for (int32 i = 0; i < p.component->getBusCount(Vst::kEvent, Vst::kInput); ++i) {
        p.component->activateBus(Vst::kEvent, Vst::kInput, i, false);
    }
    for (int32 i = 0; i < p.component->getBusCount(Vst::kEvent, Vst::kOutput); ++i) {
        p.component->activateBus(Vst::kEvent, Vst::kOutput, i, false);
    }

    Vst::ProcessSetup setup{.processMode = Vst::kRealtime,
                            .symbolicSampleSize = Vst::kSample32,
                            .maxSamplesPerBlock = maxEffectFrames,
                            .sampleRate = sampleRate};
    if (p.processor->canProcessSampleSize(Vst::kSample32) != kResultTrue ||
        p.processor->setupProcessing(setup) != kResultOk) {
        p.error = "the plugin couldn't be set up at this sample rate";
        return;
    }
    p.restoreState();
    p.initialState.clear(); // once: a later set-up (a restart) keeps what it has

    // Buffers for every bus; the main ones carry the strip.
    const auto frames = static_cast<std::size_t>(maxEffectFrames);
    p.silence.assign(frames, 0.0F);
    const auto setUp = [&](const std::vector<int32> &layout, int main, auto &buffers, auto &pointers,
                           auto &buses, bool input) {
        buffers.assign(layout.size(), {});
        pointers.assign(layout.size(), {});
        buses.assign(layout.size(), Vst::AudioBusBuffers{});
        for (std::size_t b = 0; b < layout.size(); ++b) {
            for (int32 c = 0; c < layout[b]; ++c) {
                if (input && std::cmp_not_equal(b, main)) {
                    pointers[b].push_back(p.silence.data()); // sidechains: silence
                } else {
                    pointers[b].push_back(buffers[b].emplace_back(frames).data());
                }
            }
            buses[b].numChannels = layout[b];
            buses[b].silenceFlags = 0;
            buses[b].channelBuffers32 = pointers[b].data(); // NOLINT(cppcoreguidelines-pro-type-union-access): VST3 gives 32- and 64-bit buffers a union
        }
    };
    setUp(p.inChannels, p.mainIn, p.inBuffers, p.inPointers, p.inBuses, true);
    setUp(p.outChannels, p.mainOut, p.outBuffers, p.outPointers, p.outBuses, false);
    p.in.queues.assign(p.params.size(), ParamQueue{});
    p.out.queues.assign(p.params.size() + 16, ParamQueue{});
    p.block.assign(p.params.size(), 0.0);
    p.sent.assign(p.params.size(), std::numeric_limits<double>::quiet_NaN());
    p.processContext = {};
    p.processContext.sampleRate = sampleRate;
    p.processContext.tempo = 120.0;
    p.processContext.timeSigNumerator = 4;
    p.processContext.timeSigDenominator = 4;
    // Always running (a live mixer has no stop), at 120 in 4/4.
    p.processContext.state = Vst::ProcessContext::kPlaying | Vst::ProcessContext::kTempoValid |
                             Vst::ProcessContext::kTimeSigValid | Vst::ProcessContext::kContTimeValid |
                             Vst::ProcessContext::kProjectTimeMusicValid;

    if (p.component->setActive(true) != kResultOk) {
        p.error = "the plugin couldn't be started";
        return;
    }
    p.active = true;
    p.processor->setProcessing(true);
    p.processing = true;
}

void Vst3Effect::process(float *const *channels, int channelCount, int frames) noexcept
{
    Impl &p = *d;
    // While the main thread reconfigures the plugin (a restart it asked
    // for), it's left out: the strip passes dry.
    p.inProcess.store(true);
    if (p.suspended.load() || !p.active || frames <= 0) {
        p.inProcess.store(false);
        return;
    }
    processActive(channels, channelCount, frames);
    p.inProcess.store(false);
}

void Vst3Effect::processActive(float *const *channels, int channelCount, int frames) noexcept
{
    Impl &p = *d;
    frames = std::min(frames, maxEffectFrames);
    const auto n = static_cast<std::size_t>(frames);

    // Parameters that changed since the last block.
    p.in.count = 0;
    for (std::size_t i = 0; i < p.params.size(); ++i) {
        p.block[i] = p.targets[i].load(std::memory_order_relaxed);
        if (!p.params[i].settable || p.sent[i] == p.block[i]) {
            continue;
        }
        p.sent[i] = p.block[i];
        auto &queue = p.in.queues[static_cast<std::size_t>(p.in.count++)];
        queue.id = p.params[i].id;
        queue.value = p.block[i];
        queue.points = 1;
    }
    p.out.count = 0;

    // The strip into the main input (each channel the matching one, or the
    // average of those that fall on it), and back from the main output.
    const auto spread = [n](std::span<const float *const> from, std::span<float *const> to) {
        if (from.empty() || to.empty()) {
            return;
        }
        for (std::size_t c = 0; c < to.size(); ++c) {
            float *dest = to[c];
            if (to.size() >= from.size()) {
                std::memcpy(dest, from[c % from.size()], n * sizeof(float));
                continue;
            }
            std::fill_n(dest, n, 0.0F);
            int summed = 0;
            for (std::size_t s = c; s < from.size(); s += to.size()) {
                for (std::size_t i = 0; i < n; ++i) {
                    dest[i] += from[s][i]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                }
                ++summed;
            }
            const float scale = 1.0F / static_cast<float>(std::max(summed, 1));
            for (std::size_t i = 0; i < n; ++i) {
                dest[i] *= scale; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            }
        }
    };
    const std::span<float *const> planes(channels, static_cast<std::size_t>(channelCount));
    spread(std::span<const float *const>(planes.data(), planes.size()),
           std::span<float *const>(p.inPointers[static_cast<std::size_t>(p.mainIn)]));

    p.processContext.projectTimeSamples = p.time;
    p.processContext.continousTimeSamples = p.time;
    p.processContext.projectTimeMusic = static_cast<double>(p.time) / p.processContext.sampleRate *
                                        p.processContext.tempo / 60.0; // in quarter notes
    Vst::ProcessData data;
    data.processMode = Vst::kRealtime;
    data.symbolicSampleSize = Vst::kSample32;
    data.numSamples = frames;
    data.numInputs = static_cast<int32>(p.inBuses.size());
    data.numOutputs = static_cast<int32>(p.outBuses.size());
    data.inputs = p.inBuses.data();
    data.outputs = p.outBuses.data();
    data.inputParameterChanges = &p.in;
    data.outputParameterChanges = &p.out;
    data.inputEvents = &p.events;
    data.outputEvents = nullptr;
    data.processContext = &p.processContext;
    p.time += frames;
    if (p.processor->process(data) != kResultOk) {
        // Leaves the strip as it was; this block's parameters go again next time.
        for (int32 q = 0; q < p.in.count; ++q) {
            const auto i = p.indexOf(p.in.queues[static_cast<std::size_t>(q)].id);
            if (i < p.params.size()) {
                p.sent[i] = std::numeric_limits<double>::quiet_NaN();
            }
        }
        return;
    }
    const auto &out = p.outPointers[static_cast<std::size_t>(p.mainOut)];
    spread(std::span<const float *const>(out.data(), out.size()), planes);

    // Values the plugin changed itself.
    for (int32 q = 0; q < p.out.count; ++q) {
        const auto &queue = p.out.queues[static_cast<std::size_t>(q)];
        const auto i = p.indexOf(queue.id);
        if (queue.points == 0 || i == p.params.size()) {
            continue;
        }
        p.targets[i].store(queue.value);
        p.sent[i] = queue.value;
        p.changed[i].store(true);
    }
}

void Vst3Effect::setParam(const std::string &name, double value)
{
    Vst::ParamID id = 0;
    if (std::from_chars(name.data(), name.data() + name.size(), id).ec != std::errc{} || // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        !std::isfinite(value)) {
        return;
    }
    const auto i = d->indexOf(id);
    if (i == d->params.size()) {
        return;
    }
    const int32 steps = d->params[i].steps;
    d->targets[i].store(steps > 0 ? std::clamp(value, 0.0, static_cast<double>(steps)) / steps
                                  : std::clamp(value, 0.0, 1.0));
}

void Vst3Effect::takeParamChanges(const std::function<void(const std::string &, double)> &fn)
{
    Impl &p = *d;
    for (std::size_t i = 0; i < p.params.size(); ++i) {
        // The flag first, then the value: a change landing in between is
        // either reported now or still flagged for next time.
        const bool changed = p.changed[i].exchange(false);
        const double value = p.targets[i].load();
        // The engine's values to the controller (and its editor).
        if (p.controller && !(p.controllerSent[i] == value)) {
            p.controllerSent[i] = value;
            p.controller->setParamNormalized(p.params[i].id, value);
        }
        if (changed) {
            fn(std::to_string(p.params[i].id), p.toOurs(i, value));
        }
    }
}

std::string Vst3Effect::saveState() const
{
    Impl &p = *d;
    if (!p.component) {
        return {};
    }
    MemoryStream componentState;
    MemoryStream controllerState;
    if (p.component->getState(&componentState) != kResultOk) {
        componentState.data.clear();
    }
    if (p.controller && p.separateController && p.controller->getState(&controllerState) != kResultOk) {
        controllerState.data.clear();
    }
    if (componentState.data.isEmpty() && controllerState.data.isEmpty()) {
        return {};
    }
    QByteArray out;
    for (const auto *part : {&componentState.data, &controllerState.data}) {
        const auto length = static_cast<int32>(part->size());
        out.append(reinterpret_cast<const char *>(&length), sizeof(length)); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        out.append(*part);
    }
    return out.toBase64().toStdString();
}

void Vst3Effect::setInitialState(std::string state)
{
    d->initialState = std::move(state);
}

bool Vst3Effect::takeStateDirty()
{
    return false; // asked from time to time while its window is open
}

std::unique_ptr<PluginWindow>
Vst3Effect::openWindow(const std::string &title, std::function<void(const std::string &, double)> onParam)
{
    auto window = std::make_unique<Vst3Window>(*d, title);
    window->onParam = std::move(onParam); // its edits come through performEdit, though
    return window;
}

void Vst3Effect::resume() noexcept
{
    // Back from bypass: setProcessing off and on (allowed on the audio
    // thread) lets it forget what it held (a reverb's tail).
    Impl &p = *d;
    if (p.active && p.processing && !p.suspended.load()) {
        p.processor->setProcessing(false);
        p.processor->setProcessing(true);
    }
}

void Vst3Effect::mainThreadTick()
{
    Impl &p = *d;
    const int32 flags = p.pendingRestart.exchange(0);
    if ((flags & Vst::kReloadComponent) != 0) {
        p.reload = true; // started again from scratch, by the engine
        return;
    }
    if ((flags & Vst::kParamValuesChanged) != 0) {
        p.takeOverValues();
    }
    if ((flags & (Vst::kLatencyChanged | Vst::kIoChanged)) != 0 && p.active) {
        // Set up again, with the audio thread kept out meanwhile.
        p.suspended.store(true);
        while (p.inProcess.load()) {
            std::this_thread::yield();
        }
        p.processor->setProcessing(false);
        p.processing = false;
        p.component->setActive(false);
        p.active = false;
        prepare(p.lastRate, p.lastChannels);
        p.suspended.store(false);
    }
}

bool Vst3Effect::wantsRestart() const
{
    return d->reload;
}

// --- Finding and reading bundles ----------------------------------------------

std::vector<std::string> vst3Bundles()
{
    std::vector<std::filesystem::path> dirs;
    if (const char *only = std::getenv("PATCHPRIME_VST3_PATH"); only != nullptr) { // NOLINT(concurrency-mt-unsafe): read at start-up
        dirs.emplace_back(only);
    } else {
        if (const char *home = std::getenv("HOME"); home != nullptr) { // NOLINT(concurrency-mt-unsafe): read at start-up
            dirs.emplace_back(std::filesystem::path(home) / ".vst3");
        }
        dirs.emplace_back("/usr/lib/vst3");
        dirs.emplace_back("/usr/local/lib/vst3");
    }
    std::set<std::string> seen;
    std::vector<std::string> bundles;
    for (const auto &dir : dirs) {
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator it(
                 dir, std::filesystem::directory_options::skip_permission_denied, error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
            if (it->path().extension() != ".vst3") {
                continue;
            }
            if (it->is_directory(error)) {
                it.disable_recursion_pending(); // a bundle: not looked into
            }
            auto path = std::filesystem::weakly_canonical(it->path(), error).string();
            if (seen.insert(path).second) {
                bundles.push_back(std::move(path));
            }
        }
    }
    return bundles;
}

std::string vst3Category(const std::string &subCategories)
{
    std::vector<std::string> parts;
    std::string_view rest(subCategories);
    while (!rest.empty()) {
        const auto bar = rest.find('|');
        parts.emplace_back(rest.substr(0, bar));
        rest = bar == std::string_view::npos ? std::string_view() : rest.substr(bar + 1);
    }
    static const std::vector<std::pair<std::string, std::string>> names{
        {"Instrument", "Instrument"}, {"Dynamics", "Dynamics"},   {"EQ", "Parametric EQ"},
        {"Reverb", "Reverb"},         {"Delay", "Delay"},         {"Distortion", "Distortion"},
        {"Filter", "Filter"},         {"Modulation", "Modulation"}, {"Pitch Shift", "Pitch"},
        {"Restoration", "Restoration"}, {"Analyzer", "Analyser"}, {"Mastering", "Mastering"},
        {"Spatial", "Spatial"},       {"Surround", "Spatial"},    {"Tools", "Utility"},
        {"Generator", "Instrument"},
    };
    for (const auto &[name, category] : names) {
        if (std::ranges::find(parts, name) != parts.end()) {
            return category;
        }
    }
    return "Other";
}

std::vector<PluginInfo> scanVst3Bundle(const std::string &path)
{
    std::vector<PluginInfo> out;
    std::string error;
    const auto factory = loadFactory(path, error);
    if (!factory) {
        std::println(stderr, "patchprime-pluginhost: {}: {}", path, error);
        return out;
    }
    const auto factory2 = query<IPluginFactory2>(factory);
    PFactoryInfo factoryInfo{};
    factory->getFactoryInfo(&factoryInfo);
    for (int32 i = 0; i < factory->countClasses(); ++i) {
        PClassInfo info{};
        if (factory->getClassInfo(i, &info) != kResultOk ||
            std::strcmp(info.category, kVstAudioEffectClass) != 0) {
            continue;
        }
        std::array<char8, 33> hex{};
        FUID::fromTUID(info.cid).toString(hex.data());
        PluginInfo p;
        p.type = std::string(vst3TypePrefix) + hex.data();
        p.name = text(info.name, PClassInfo::kNameSize);
        p.author = text(factoryInfo.vendor, PFactoryInfo::kNameSize);
        p.binary = path;
        std::string sub;
        if (PClassInfo2 info2{}; factory2 && factory2->getClassInfo2(i, &info2) == kResultOk) {
            sub = text(info2.subCategories, PClassInfo2::kSubCategoriesSize);
            if (info2.vendor[0] != 0) {
                p.author = text(info2.vendor, PClassInfo2::kVendorSize);
            }
        }
        p.category = vst3Category(sub);

        Vst3Effect::Impl plugin;
        if (!plugin.create(factory, info.cid)) {
            p.problem = plugin.error;
            out.push_back(std::move(p));
            continue;
        }
        for (const auto &param : plugin.params) {
            if (!param.settable) {
                continue;
            }
            Vst::ParameterInfo pi{};
            const auto at = static_cast<int32>(&param - plugin.params.data());
            plugin.controller->getParameterInfo(at, pi);
            ParamInfo info3;
            info3.name = std::to_string(param.id);
            info3.label = utf8(pi.title);
            if (param.steps > 0) {
                info3.kind = param.steps == 1 ? ParamKind::Toggle : ParamKind::Choice;
                info3.maximum = param.steps;
                info3.initial = std::round(param.initial * param.steps);
                if (param.steps > 1) {
                    for (int32 s = 0; s <= param.steps; ++s) {
                        Vst::String128 text{};
                        plugin.controller->getParamStringByValue(param.id, static_cast<double>(s) / param.steps, text);
                        info3.options.push_back(utf8(text));
                    }
                }
            } else {
                info3.maximum = 1.0;
                info3.initial = param.initial;
            }
            p.params.push_back(std::move(info3));
        }
        for (const bool input : {true, false}) {
            const auto dir = input ? Vst::kInput : Vst::kOutput;
            for (int32 b = 0; b < plugin.component->getBusCount(Vst::kAudio, dir); ++b) {
                Vst::BusInfo bus{};
                if (plugin.component->getBusInfo(Vst::kAudio, dir, b, bus) == kResultOk &&
                    bus.busType == Vst::kMain) {
                    (input ? p.audioIn : p.audioOut) = bus.channelCount;
                    break;
                }
            }
        }
        // Whether it has an editor: making one to find out is slow (LSP
        // lays out its whole window), so any controller counts; opening the
        // window says when there's none after all.
        p.hasUi = static_cast<bool>(plugin.controller);
        if (p.audioIn == 0 || p.audioOut == 0 || p.category == "Instrument") {
            p.problem = "it doesn't process audio (an instrument, or an analyser)";
        }
        out.push_back(std::move(p));
    }
    return out;
}

} // namespace pp

// NOLINTEND(readability-implicit-bool-conversion,cppcoreguidelines-pro-bounds-array-to-pointer-decay)
