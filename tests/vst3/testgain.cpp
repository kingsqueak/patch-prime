// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 KC2RGW Chris

// A tiny VST3 plugin for the tests, like tests/lv2/testgain.c: mono gain, a
// "crash" switch that kills its process, and a "halve" switch that halves
// its output for good (a trim kept only in its state), turning itself off
// again through an output parameter change, as a plugin's editor would. One
// object is both its component and its edit controller.

#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <atomic>
#include <csignal>
#include <cstring>

using namespace Steinberg; // NOLINT(google-build-using-namespace)

// Written as a VST3 plugin is: the API's char-array names, its buffer union,
// and indexing by parameter id.
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-array-to-pointer-decay,cppcoreguidelines-pro-type-union-access,cppcoreguidelines-pro-bounds-constant-array-index)

namespace {

const FUID testGainId(0x50505450, 0x54657374, 0x4761696E, 0x56535433);
constexpr Vst::ParamID gainId = 1;
constexpr Vst::ParamID crashId = 2;
constexpr Vst::ParamID halveId = 3;

void copy(Vst::String128 to, const char16_t *from)
{
    int i = 0;
    for (; from[i] != 0 && i < 127; ++i) {
        to[i] = from[i];
    }
    to[i] = 0;
}

class TestGain : public U::Implements<U::Directly<Vst::IComponent, Vst::IAudioProcessor, Vst::IEditController>>
{
public:
    // IPluginBase (both IComponent's and IEditController's)
    tresult PLUGIN_API initialize(FUnknown * /*context*/) override { return kResultOk; }
    tresult PLUGIN_API terminate() override { return kResultOk; }

    // IComponent
    tresult PLUGIN_API getControllerClassId(TUID /*classId*/) override { return kNotImplemented; }
    tresult PLUGIN_API setIoMode(Vst::IoMode /*mode*/) override { return kResultOk; }
    int32 PLUGIN_API getBusCount(Vst::MediaType type, Vst::BusDirection /*dir*/) override
    {
        return type == Vst::kAudio ? 1 : 0;
    }
    tresult PLUGIN_API getBusInfo(Vst::MediaType type, Vst::BusDirection dir, int32 index, Vst::BusInfo &bus) override
    {
        if (type != Vst::kAudio || index != 0) {
            return kInvalidArgument;
        }
        bus.mediaType = Vst::kAudio;
        bus.direction = dir;
        bus.channelCount = 1;
        copy(bus.name, dir == Vst::kInput ? u"In" : u"Out");
        bus.busType = Vst::kMain;
        bus.flags = Vst::BusInfo::kDefaultActive;
        return kResultOk;
    }
    tresult PLUGIN_API getRoutingInfo(Vst::RoutingInfo & /*in*/, Vst::RoutingInfo & /*out*/) override
    {
        return kNotImplemented;
    }
    tresult PLUGIN_API activateBus(Vst::MediaType /*type*/, Vst::BusDirection /*dir*/, int32 /*index*/,
                                   TBool /*state*/) override
    {
        return kResultOk;
    }
    tresult PLUGIN_API setActive(TBool /*state*/) override { return kResultOk; }
    tresult PLUGIN_API setState(IBStream *state) override
    {
        float trim = 1.0F;
        int32 read = 0;
        if (state != nullptr && state->read(&trim, sizeof(trim), &read) == kResultOk && read == sizeof(trim)) {
            m_trim.store(trim);
        }
        return kResultOk;
    }
    tresult PLUGIN_API getState(IBStream *state) override
    {
        const float trim = m_trim.load();
        return state != nullptr ? state->write(const_cast<float *>(&trim), sizeof(trim), nullptr) : kResultFalse; // NOLINT(cppcoreguidelines-pro-type-const-cast): IBStream isn't const
    }

    // IAudioProcessor: mono only.
    tresult PLUGIN_API setBusArrangements(Vst::SpeakerArrangement *inputs, int32 numIns,
                                          Vst::SpeakerArrangement *outputs, int32 numOuts) override
    {
        return numIns == 1 && numOuts == 1 && inputs[0] == Vst::SpeakerArr::kMono &&
                       outputs[0] == Vst::SpeakerArr::kMono
                   ? kResultTrue
                   : kResultFalse;
    }
    tresult PLUGIN_API getBusArrangement(Vst::BusDirection /*dir*/, int32 index, Vst::SpeakerArrangement &arr) override
    {
        if (index != 0) {
            return kInvalidArgument;
        }
        arr = Vst::SpeakerArr::kMono;
        return kResultOk;
    }
    tresult PLUGIN_API canProcessSampleSize(int32 size) override
    {
        return size == Vst::kSample32 ? kResultTrue : kResultFalse;
    }
    uint32 PLUGIN_API getLatencySamples() override { return 0; }
    tresult PLUGIN_API setupProcessing(Vst::ProcessSetup & /*setup*/) override { return kResultOk; }
    tresult PLUGIN_API setProcessing(TBool /*state*/) override { return kResultOk; }
    tresult PLUGIN_API process(Vst::ProcessData &data) override
    {
        if (data.inputParameterChanges != nullptr) {
            for (int32 i = 0; i < data.inputParameterChanges->getParameterCount(); ++i) {
                Vst::IParamValueQueue *queue = data.inputParameterChanges->getParameterData(i);
                int32 offset = 0;
                Vst::ParamValue value = 0;
                const int32 points = queue != nullptr ? queue->getPointCount() : 0;
                if (points > 0 && queue->getPoint(points - 1, offset, value) == kResultOk) {
                    set(queue->getParameterId(), value);
                }
            }
        }
        if (m_values[crashId] > 0.5) {
            std::raise(SIGKILL);
        }
        if (m_values[halveId] > 0.5) {
            m_trim.store(0.5F);
            m_values[halveId] = 0.0;
            if (data.outputParameterChanges != nullptr) {
                int32 index = 0;
                if (Vst::IParamValueQueue *queue = data.outputParameterChanges->addParameterData(halveId, index)) {
                    queue->addPoint(0, 0.0, index);
                }
            }
        }
        if (data.numInputs < 1 || data.numOutputs < 1 || data.inputs[0].numChannels < 1 ||
            data.outputs[0].numChannels < 1) {
            return kResultOk;
        }
        const float gain = static_cast<float>(m_values[gainId] * 4.0) * m_trim.load();
        const float *in = data.inputs[0].channelBuffers32[0];
        float *out = data.outputs[0].channelBuffers32[0];
        for (int32 i = 0; i < data.numSamples; ++i) {
            out[i] = in[i] * gain;
        }
        return kResultOk;
    }
    uint32 PLUGIN_API getTailSamples() override { return 0; }

    // IEditController
    tresult PLUGIN_API setComponentState(IBStream * /*state*/) override { return kResultOk; }
    int32 PLUGIN_API getParameterCount() override { return 3; }
    tresult PLUGIN_API getParameterInfo(int32 index, Vst::ParameterInfo &info) override
    {
        if (index < 0 || index > 2) {
            return kInvalidArgument;
        }
        info = {};
        info.id = static_cast<Vst::ParamID>(index + 1);
        info.flags = Vst::ParameterInfo::kCanAutomate;
        if (info.id == gainId) {
            copy(info.title, u"Gain");
            info.defaultNormalizedValue = 0.25; // x1
        } else {
            copy(info.title, info.id == crashId ? u"Crash" : u"Halve");
            info.stepCount = 1;
        }
        return kResultOk;
    }
    tresult PLUGIN_API getParamStringByValue(Vst::ParamID /*id*/, Vst::ParamValue /*value*/,
                                             Vst::String128 string) override
    {
        copy(string, u"");
        return kResultOk;
    }
    tresult PLUGIN_API getParamValueByString(Vst::ParamID /*id*/, Vst::TChar * /*string*/,
                                             Vst::ParamValue & /*value*/) override
    {
        return kNotImplemented;
    }
    Vst::ParamValue PLUGIN_API normalizedParamToPlain(Vst::ParamID /*id*/, Vst::ParamValue value) override
    {
        return value;
    }
    Vst::ParamValue PLUGIN_API plainParamToNormalized(Vst::ParamID /*id*/, Vst::ParamValue value) override
    {
        return value;
    }
    Vst::ParamValue PLUGIN_API getParamNormalized(Vst::ParamID id) override
    {
        return id >= gainId && id <= halveId ? m_values[id] : 0.0;
    }
    tresult PLUGIN_API setParamNormalized(Vst::ParamID /*id*/, Vst::ParamValue /*value*/) override
    {
        return kResultOk; // the processor's values come as parameter changes
    }
    tresult PLUGIN_API setComponentHandler(Vst::IComponentHandler * /*handler*/) override { return kResultOk; }
    IPlugView *PLUGIN_API createView(FIDString /*name*/) override { return nullptr; }

private:
    void set(Vst::ParamID id, double value)
    {
        if (id >= gainId && id <= halveId) {
            m_values[id] = value;
        }
    }

    double m_values[4] = {0.0, 0.25, 0.0, 0.0}; // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
    std::atomic<float> m_trim{1.0F}; // written by process, read by getState
};

class Factory : public U::Implements<U::Directly<IPluginFactory2>, U::Indirectly<IPluginFactory>>
{
public:
    tresult PLUGIN_API getFactoryInfo(PFactoryInfo *info) override
    {
        *info = {};
        std::strcpy(info->vendor, "Patch Prime tests");
        return kResultOk;
    }
    int32 PLUGIN_API countClasses() override { return 1; }
    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo *info) override
    {
        if (index != 0) {
            return kInvalidArgument;
        }
        *info = {};
        testGainId.toTUID(info->cid);
        info->cardinality = PClassInfo::kManyInstances;
        std::strcpy(info->category, kVstAudioEffectClass);
        std::strcpy(info->name, "Patch Prime Test Gain (VST3)");
        return kResultOk;
    }
    tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2 *info) override
    {
        if (index != 0) {
            return kInvalidArgument;
        }
        *info = {};
        testGainId.toTUID(info->cid);
        info->cardinality = PClassInfo::kManyInstances;
        std::strcpy(info->category, kVstAudioEffectClass);
        std::strcpy(info->name, "Patch Prime Test Gain (VST3)");
        std::strcpy(info->subCategories, "Fx|Tools");
        std::strcpy(info->vendor, "Patch Prime tests");
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(FIDString cid, FIDString iid, void **obj) override
    {
        *obj = nullptr;
        if (FUID::fromTUID(*reinterpret_cast<const TUID *>(cid)) != testGainId) { // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
            return kNoInterface;
        }
        auto *plugin = new TestGain; // one reference, ours
        const tresult result = plugin->queryInterface(*reinterpret_cast<const TUID *>(iid), obj); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        plugin->release(); // the caller's (if it got one) is all that's left
        return result;
    }
};

} // namespace

extern "C" {

SMTG_EXPORT_SYMBOL bool ModuleEntry(void * /*library*/)
{
    return true;
}

SMTG_EXPORT_SYMBOL bool ModuleExit()
{
    return true;
}

SMTG_EXPORT_SYMBOL IPluginFactory *PLUGIN_API GetPluginFactory()
{
    return new Factory; // the host releases it
}

} // extern "C"

// NOLINTEND(cppcoreguidelines-pro-bounds-array-to-pointer-decay,cppcoreguidelines-pro-type-union-access,cppcoreguidelines-pro-bounds-constant-array-index)
