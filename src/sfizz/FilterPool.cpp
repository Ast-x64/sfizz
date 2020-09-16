#include "FilterPool.h"
#include "SIMDHelpers.h"
#include "SwapAndPop.h"
#include "absl/algorithm/container.h"
#include <thread>
#include <chrono>

sfz::FilterHolder::FilterHolder(Resources& resources)
: resources(resources)
{
    filter = absl::make_unique<Filter>();
    filter->init(config::defaultSampleRate);
}

void sfz::FilterHolder::reset()
{
    filter->clear();
}

void sfz::FilterHolder::setup(const Region& region, unsigned filterId, int noteNumber, float velocity)
{
    ASSERT(velocity >= 0.0f && velocity <= 1.0f);
    ASSERT(filterId < region.filters.size());

    this->description = &region.filters[filterId];
    filter->setType(description->type);
    filter->setChannels(region.isStereo() ? 2 : 1);

    // Setup the base values
    baseCutoff = description->cutoff;
    if (description->random != 0) {
       dist.param(filterRandomDist::param_type(0, description->random));
       baseCutoff *= centsFactor(dist(Random::randomGenerator));
    }
    const auto keytrack = description->keytrack * (noteNumber - description->keycenter);
    baseCutoff *= centsFactor(keytrack);
    const auto veltrack = static_cast<float>(description->veltrack) * velocity;
    baseCutoff *= centsFactor(veltrack);
    baseCutoff = Default::filterCutoffRange.clamp(baseCutoff);

    baseGain = description->gain;
    baseResonance = description->resonance;

    // Setup the modulated values
    float lastCutoff = baseCutoff;
    for (const auto& mod : description->cutoffCC)
        lastCutoff *= centsFactor(resources.midiState.getCCValue(mod.cc) * mod.data);
    lastCutoff = Default::filterCutoffRange.clamp(lastCutoff);

    float lastResonance = baseResonance;
    for (const auto& mod : description->resonanceCC)
        lastResonance += resources.midiState.getCCValue(mod.cc) * mod.data;
    lastResonance = Default::filterResonanceRange.clamp(lastResonance);

    float lastGain = baseGain;
    for (const auto& mod : description->gainCC)
        lastGain += resources.midiState.getCCValue(mod.cc) * mod.data;
    lastGain = Default::filterGainRange.clamp(lastGain);

    ModMatrix& mm = resources.modMatrix;
    gainTarget = mm.findTarget(ModKey::createNXYZ(ModId::FilGain, region.id, filterId));
    cutoffTarget = mm.findTarget(ModKey::createNXYZ(ModId::FilCutoff, region.id, filterId));
    resonanceTarget = mm.findTarget(ModKey::createNXYZ(ModId::FilResonance, region.id, filterId));

    // Initialize the filter
    filter->prepare(lastCutoff, lastResonance, lastGain);
}

void sfz::FilterHolder::process(const float** inputs, float** outputs, unsigned numFrames)
{
    if (description == nullptr) {
        for (unsigned channelIdx = 0; channelIdx < filter->channels(); channelIdx++)
            copy<float>({ inputs[channelIdx], numFrames }, { outputs[channelIdx], numFrames });
        return;
    }

    ModMatrix& mm = resources.modMatrix;
    auto cutoffSpan = resources.bufferPool.getBuffer(numFrames);
    auto resonanceSpan = resources.bufferPool.getBuffer(numFrames);
    auto gainSpan = resources.bufferPool.getBuffer(numFrames);

    if (!cutoffSpan || !resonanceSpan || !gainSpan)
        return;

    fill<float>(*cutoffSpan, baseCutoff);
    if (float* mod = mm.getModulation(cutoffTarget)) {
        for (size_t i = 0; i < numFrames; ++i)
            (*cutoffSpan)[i] *= centsFactor(mod[i]);
    }

    fill<float>(*resonanceSpan, baseResonance);
    if (float* mod = mm.getModulation(resonanceTarget))
        add<float>(absl::Span<float>(mod, numFrames), *resonanceSpan);

    fill<float>(*gainSpan, baseGain);
    if (float* mod = mm.getModulation(gainTarget))
        add<float>(absl::Span<float>(mod, numFrames), *gainSpan);

    filter->processModulated(
        inputs,
        outputs,
        cutoffSpan->data(),
        resonanceSpan->data(),
        gainSpan->data(),
        numFrames
    );
}


void sfz::FilterHolder::setSampleRate(float sampleRate)
{
    filter->init(static_cast<double>(sampleRate));
}
