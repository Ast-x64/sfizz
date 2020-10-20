// SPDX-License-Identifier: BSD-2-Clause

// Copyright (c) 2019-2020, Paul Ferrand, Andrea Zanellato
// All rights reserved.

// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:

// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.

// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "FilePool.h"
#include "AudioReader.h"
#include "Buffer.h"
#include "AudioBuffer.h"
#include "AudioSpan.h"
#include "SwapAndPop.h"
#include "Config.h"
#include "Debug.h"
#include "Oversampler.h"
#include "absl/types/span.h"
#include "absl/strings/match.h"
#include "absl/memory/memory.h"
#include <algorithm>
#include <memory>
#include <thread>
#include <system_error>
#include <sndfile.hh>
#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif
#include "threadpool/ThreadPool.h"
using namespace std::placeholders;

static std::weak_ptr<ThreadPool> globalThreadPoolWeakPtr;
static std::mutex globalThreadPoolMutex;

static std::shared_ptr<ThreadPool> globalThreadPool()
{
    std::shared_ptr<ThreadPool> threadPool;

    threadPool = globalThreadPoolWeakPtr.lock();
    if (threadPool)
        return threadPool;

    std::lock_guard<std::mutex> lock(globalThreadPoolMutex);
    threadPool = globalThreadPoolWeakPtr.lock();
    if (threadPool)
        return threadPool;

    unsigned numThreads = std::thread::hardware_concurrency();
    numThreads = (numThreads > 2) ? (numThreads - 2) : 1;
    threadPool.reset(new ThreadPool(numThreads));
    globalThreadPoolWeakPtr = threadPool;
    return threadPool;
}

void readBaseFile(sfz::AudioReader& reader, sfz::FileAudioBuffer& output, uint32_t numFrames)
{
    output.reset();
    output.resize(numFrames);

    const unsigned channels = reader.channels();

    if (channels == 1) {
        output.addChannel();
        output.clear();
        reader.readNextBlock(output.channelWriter(0), numFrames);
    } else if (channels == 2) {
        output.addChannel();
        output.addChannel();
        output.clear();
        sfz::Buffer<float> tempReadBuffer { 2 * numFrames };
        reader.readNextBlock(tempReadBuffer.data(), numFrames);
        sfz::readInterleaved(tempReadBuffer, output.getSpan(0), output.getSpan(1));
    }
}

sfz::FileAudioBuffer readFromFile(sfz::AudioReader& reader, uint32_t numFrames, sfz::Oversampling factor)
{
    sfz::FileAudioBuffer baseBuffer;
    readBaseFile(reader, baseBuffer, numFrames);

    if (factor == sfz::Oversampling::x1)
        return baseBuffer;

    sfz::FileAudioBuffer outputBuffer { reader.channels(), numFrames * static_cast<int>(factor) };
    outputBuffer.clear();
    sfz::Oversampler oversampler { factor };
    oversampler.stream(baseBuffer, outputBuffer);
    return outputBuffer;
}

void streamFromFile(sfz::AudioReader& reader, uint32_t numFrames, sfz::Oversampling factor, sfz::FileAudioBuffer& output, std::atomic<size_t>* filledFrames = nullptr)
{
    output.reset();
    output.addChannels(reader.channels());
    output.resize(numFrames * static_cast<int>(factor));
    output.clear();
    sfz::Oversampler oversampler { factor };
    oversampler.stream(reader, output, filledFrames);
}

sfz::FilePool::FilePool(sfz::Logger& logger)
    : logger(logger), threadPool(globalThreadPool())
{
    loadingJobs.reserve(config::maxVoices);
    lastUsedFiles.reserve(config::maxVoices);
    garbageToCollect.reserve(config::maxVoices);
}

sfz::FilePool::~FilePool()
{
    std::error_code ec;

    garbageFlag = false;
    semGarbageBarrier.post(ec);
    garbageThread.join();

    dispatchFlag = false;
    dispatchBarrier.post(ec);
    dispatchThread.join();

    for (auto& job : loadingJobs)
        job.wait();
}

bool sfz::FilePool::checkSample(std::string& filename) const noexcept
{
    fs::path path { rootDirectory / filename };
    std::error_code ec;
    if (fs::exists(path, ec))
        return true;

#if defined(_WIN32)
    return false;
#else
    fs::path oldPath = std::move(path);
    path = oldPath.root_path();

    static const fs::path dot { "." };
    static const fs::path dotdot { ".." };

    for (const fs::path& part : oldPath.relative_path()) {
        if (part == dot || part == dotdot) {
            path /= part;
            continue;
        }

        if (fs::exists(path / part, ec)) {
            path /= part;
            continue;
        }

        auto it = path.empty() ? fs::directory_iterator { dot, ec } : fs::directory_iterator { path, ec };
        if (ec) {
            DBG("Error creating a directory iterator for " << filename << " (Error code: " << ec.message() << ")");
            return false;
        }

        auto searchPredicate = [&part](const fs::directory_entry &ent) -> bool {
#if !defined(GHC_USE_WCHAR_T)
            return absl::EqualsIgnoreCase(
                ent.path().filename().native(), part.native());
#else
            return absl::EqualsIgnoreCase(
                ent.path().filename().u8string(), part.u8string());
#endif
        };

        while (it != fs::directory_iterator {} && !searchPredicate(*it))
            it.increment(ec);

        if (it == fs::directory_iterator {}) {
            DBG("File not found, could not resolve " << filename);
            return false;
        }

        path /= it->path().filename();
    }

    const auto newPath = fs::relative(path, rootDirectory, ec);
    if (ec) {
        DBG("Error extracting the new relative path for " << filename << " (Error code: " << ec.message() << ")");
        return false;
    }
    DBG("Updating " << filename << " to " << newPath);
    filename = newPath.string();
    return true;
#endif
}

bool sfz::FilePool::checkSampleId(FileId& fileId) const noexcept
{
    std::string filename = fileId.filename();
    bool result = checkSample(filename);
    if (result)
        fileId = FileId(std::move(filename), fileId.isReverse());
    return result;
}

absl::optional<sfz::FileInformation> sfz::FilePool::getFileInformation(const FileId& fileId) noexcept
{
    const fs::path file { rootDirectory / fileId.filename() };

    if (!fs::exists(file))
        return {};

    AudioReaderPtr reader = createAudioReader(file, fileId.isReverse());
    const unsigned channels = reader->channels();

    if (channels != 1 && channels != 2) {
        DBG("[sfizz] Missing logic for " << reader->channels() << " channels, discarding sample " << fileId);
        return {};
    }

    FileInformation returnedValue;
    returnedValue.end = static_cast<uint32_t>(reader->frames()) - 1;
    returnedValue.sampleRate = static_cast<double>(reader->sampleRate());
    returnedValue.numChannels = reader->channels();

    SF_INSTRUMENT instrumentInfo {};
    bool haveInstrumentInfo = reader->getInstrument(&instrumentInfo);

    FileMetadataReader mdReader;
    bool mdReaderOpened = mdReader.open(file);

    if (!haveInstrumentInfo) {
        // if no instrument, then try extracting from embedded RIFF chunks (flac)
        if (mdReaderOpened)
            haveInstrumentInfo = mdReader.extractRiffInstrument(instrumentInfo);
    }

    if (mdReaderOpened) {
        WavetableInfo wt;
        if (mdReader.extractWavetableInfo(wt))
            returnedValue.wavetable = wt;
    }

    if (!fileId.isReverse()) {
        if (haveInstrumentInfo && instrumentInfo.loop_count > 0) {
            returnedValue.hasLoop = true;
            returnedValue.loopBegin = instrumentInfo.loops[0].start;
            returnedValue.loopEnd = min(returnedValue.end, instrumentInfo.loops[0].end - 1);
        }
    } else {
        // TODO loops ignored when reversed
        //   prehaps it can make use of SF_LOOP_BACKWARD?
    }

    if (haveInstrumentInfo)
        returnedValue.rootKey = clamp<int8_t>(instrumentInfo.basenote, 0, 127);

    return returnedValue;
}

bool sfz::FilePool::preloadFile(const FileId& fileId, uint32_t maxOffset) noexcept
{
    auto fileInformation = getFileInformation(fileId);
    if (!fileInformation)
        return false;

    fileInformation->maxOffset = maxOffset;
    const fs::path file { rootDirectory / fileId.filename() };
    AudioReaderPtr reader = createAudioReader(file, fileId.isReverse());

    const auto frames = static_cast<uint32_t>(reader->frames());
    const auto framesToLoad = [&]() {
        if (loadInRam)
            return frames;
        else
            return min(frames, maxOffset + preloadSize);
    }();

    const auto existingFile = preloadedFiles.find(fileId);
    if (existingFile != preloadedFiles.end()) {
        if (framesToLoad > existingFile->second.preloadedData.getNumFrames()) {
            preloadedFiles[fileId].information.maxOffset = maxOffset;
            preloadedFiles[fileId].preloadedData = readFromFile(*reader, framesToLoad, oversamplingFactor);
        }
    } else {
        const auto factor = static_cast<double>(oversamplingFactor);
        fileInformation->sampleRate = factor * static_cast<double>(reader->sampleRate());
        fileInformation->end = static_cast<uint32_t>(factor * fileInformation->end);
        fileInformation->loopBegin = static_cast<uint32_t>(factor * fileInformation->loopBegin);
        fileInformation->loopEnd = static_cast<uint32_t>(factor * fileInformation->loopEnd);
        auto insertedPair = preloadedFiles.insert_or_assign(fileId, {
            readFromFile(*reader, framesToLoad, oversamplingFactor),
            *fileInformation
        });

        if (!insertedPair.second)
            return false;

        insertedPair.first->second.status = FileData::Status::Preloaded;
    }
    return true;
}

sfz::FileDataHolder sfz::FilePool::loadFile(const FileId& fileId) noexcept
{
    auto fileInformation = getFileInformation(fileId);
    if (!fileInformation)
        return {};

    const fs::path file { rootDirectory / fileId.filename() };
    AudioReaderPtr reader = createAudioReader(file, fileId.isReverse());

    const auto frames = static_cast<uint32_t>(reader->frames());
    const auto existingFile = loadedFiles.find(fileId);
    if (existingFile != loadedFiles.end()) {
        return { &existingFile->second };
    } else {
        const auto factor = static_cast<double>(oversamplingFactor);
        fileInformation->sampleRate = factor * static_cast<double>(reader->sampleRate());
        fileInformation->end = static_cast<uint32_t>(factor * fileInformation->end);
        fileInformation->loopBegin = static_cast<uint32_t>(factor * fileInformation->loopBegin);
        fileInformation->loopEnd = static_cast<uint32_t>(factor * fileInformation->loopEnd);
        auto insertedPair = preloadedFiles.insert_or_assign(fileId, {
            readFromFile(*reader, frames, oversamplingFactor),
            *fileInformation
        });
        insertedPair.first->second.status = FileData::Status::Preloaded;
        ASSERT(insertedPair.second);
        return { &insertedPair.first->second };
    }
}

sfz::FileDataHolder sfz::FilePool::getFilePromise(const std::shared_ptr<FileId>& fileId) noexcept
{
    const auto preloaded = preloadedFiles.find(*fileId);
    if (preloaded == preloadedFiles.end()) {
        DBG("[sfizz] File not found in the preloaded files: " << fileId);
        return {};
    }
    QueuedFileData queuedData { fileId, &preloaded->second, std::chrono::high_resolution_clock::now() };
    if (!filesToLoad.try_push(queuedData)) {
        DBG("[sfizz] Could not enqueue the file to load for " << fileId << " (queue capacity " << filesToLoad.capacity() << ")");
        return {};
    }

    std::error_code ec;
    dispatchBarrier.post(ec);
    ASSERT(!ec);

    return { &preloaded->second };
}

void sfz::FilePool::setPreloadSize(uint32_t preloadSize) noexcept
{
    this->preloadSize = preloadSize;
    if (loadInRam)
        return;

    // Update all the preloaded sizes
    for (auto& preloadedFile : preloadedFiles) {
        const auto maxOffset = preloadedFile.second.information.maxOffset;
        fs::path file { rootDirectory / preloadedFile.first.filename() };
        AudioReaderPtr reader = createAudioReader(file, preloadedFile.first.isReverse());
        preloadedFile.second.preloadedData = readFromFile(*reader, preloadSize + maxOffset, oversamplingFactor);
    }
}

void sfz::FilePool::loadingJob(QueuedFileData data) noexcept
{
    raiseCurrentThreadPriority();

    std::shared_ptr<FileId> id = data.id.lock();
    if (!id) {
        // file ID was nulled, it means the region was deleted, ignore
        return;
    }

    const auto loadStartTime = std::chrono::high_resolution_clock::now();
    const auto waitDuration = loadStartTime - data.queuedTime;
    const fs::path file { rootDirectory / id->filename() };
    std::error_code readError;
    AudioReaderPtr reader = createAudioReader(file, id->isReverse(), &readError);

    if (readError) {
        DBG("[sfizz] libsndfile errored for " << *id << " with message " << readError.message());
        return;
    }

    FileData::Status currentStatus = data.data->status.load();

    unsigned spinCounter { 0 };
    while (currentStatus == FileData::Status::Invalid) {
        // Spin until the state changes
        if (spinCounter > 1024) {
            DBG("[sfizz] " << *id << " is stuck on Invalid? Leaving the load");
            return;
        }

        std::this_thread::sleep_for(std::chrono::microseconds(100));
        currentStatus = data.data->status.load();
        spinCounter += 1;
    }

    // Already loading or loaded
    if (currentStatus != FileData::Status::Preloaded)
        return;

    // Someone else got the token
    if (!data.data->status.compare_exchange_strong(currentStatus, FileData::Status::Streaming))
        return;

    const auto frames = static_cast<uint32_t>(reader->frames());
    streamFromFile(*reader, frames, oversamplingFactor, data.data->fileData, &data.data->availableFrames);
    const auto loadDuration = std::chrono::high_resolution_clock::now() - loadStartTime;
    logger.logFileTime(waitDuration, loadDuration, frames, id->filename());

    data.data->status = FileData::Status::Done;

    std::lock_guard<SpinMutex> guard { lastUsedMutex };
    if (absl::c_find(lastUsedFiles, *id) == lastUsedFiles.end())
        lastUsedFiles.push_back(*id);
}

void sfz::FilePool::clear()
{
    emptyFileLoadingQueues();
    preloadedFiles.clear();
}

void sfz::FilePool::setOversamplingFactor(sfz::Oversampling factor) noexcept
{
    float samplerateChange { static_cast<float>(factor) / static_cast<float>(this->oversamplingFactor) };
    for (auto& preloadedFile : preloadedFiles) {
        const auto framesToLoad = [&]() {
            if (loadInRam)
                return preloadedFile.second.information.end;
            else
                return min(
                    preloadedFile.second.information.end,
                    preloadedFile.second.information.maxOffset + preloadSize
                );
        }();

        fs::path file { rootDirectory / preloadedFile.first.filename() };
        AudioReaderPtr reader = createAudioReader(file, preloadedFile.first.isReverse());
        preloadedFile.second.preloadedData = readFromFile(*reader, framesToLoad, factor);
        FileInformation& information = preloadedFile.second.information;
        information.sampleRate *= samplerateChange;
        information.end = static_cast<uint32_t>(samplerateChange * information.end);
        information.loopBegin = static_cast<uint32_t>(samplerateChange * information.loopBegin);
        information.loopEnd = static_cast<uint32_t>(samplerateChange * information.loopEnd);

        if (preloadedFile.second.status == FileData::Status::Done) {
            const auto realFrames =
                preloadedFile.second.availableFrames.load() / static_cast<unsigned>(this->oversamplingFactor);
            preloadedFile.second.fileData = readFromFile(*reader, realFrames, factor);
            preloadedFile.second.availableFrames = realFrames * static_cast<unsigned>(factor);
        }
    }

    this->oversamplingFactor = factor;
}

sfz::Oversampling sfz::FilePool::getOversamplingFactor() const noexcept
{
    return oversamplingFactor;
}

uint32_t sfz::FilePool::getPreloadSize() const noexcept
{
    return preloadSize;
}

template <typename R>
bool is_ready(std::future<R> const& f)
{
    return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

void sfz::FilePool::dispatchingJob() noexcept
{
    QueuedFileData queuedData;
    while (dispatchBarrier.wait(), dispatchFlag) {
        std::lock_guard<std::mutex> guard { loadingJobsMutex };

        if (filesToLoad.try_pop(queuedData)) {
            if (!queuedData.id.lock()) {
                // file ID was nulled, it means the region was deleted, ignore
            }
            else
                loadingJobs.push_back(
                    threadPool->enqueue([this](const QueuedFileData& data) { loadingJob(data); }, queuedData));
        }

        // Clear finished jobs
        swapAndPopAll(loadingJobs, [](std::future<void>& future) {
            return is_ready(future);
        });
    }
}

void sfz::FilePool::garbageJob() noexcept
{
    while (semGarbageBarrier.wait(), garbageFlag) {
        std::lock_guard<SpinMutex> guard { garbageMutex };
        for (auto& g: garbageToCollect)
            g.reset();

        garbageToCollect.clear();
    }
}

void sfz::FilePool::waitForBackgroundLoading() noexcept
{
    std::lock_guard<std::mutex> guard { loadingJobsMutex };

    for (auto& job : loadingJobs)
        job.wait();

    loadingJobs.clear();
}

void sfz::FilePool::raiseCurrentThreadPriority() noexcept
{
#if defined(_WIN32)
    HANDLE thread = GetCurrentThread();
    const int priority = THREAD_PRIORITY_ABOVE_NORMAL; /*THREAD_PRIORITY_HIGHEST*/
    if (!SetThreadPriority(thread, priority)) {
        std::system_error error(GetLastError(), std::system_category());
        DBG("[sfizz] Cannot set current thread priority: " << error.what());
    }
#else
    pthread_t thread = pthread_self();
    int policy;
    sched_param param;

    if (pthread_getschedparam(thread, &policy, &param) != 0) {
        DBG("[sfizz] Cannot get current thread scheduling parameters");
        return;
    }

    policy = SCHED_RR;
    const int minprio = sched_get_priority_min(policy);
    const int maxprio = sched_get_priority_max(policy);
    param.sched_priority = minprio + config::backgroundLoaderPthreadPriority * (maxprio - minprio) / 100;

    if (pthread_setschedparam(thread, policy, &param) != 0) {
        DBG("[sfizz] Cannot set current thread scheduling parameters");
        return;
    }
#endif
}

void sfz::FilePool::setRamLoading(bool loadInRam) noexcept
{
    if (loadInRam == this->loadInRam)
        return;

    this->loadInRam = loadInRam;

    if (loadInRam) {
        for (auto& preloadedFile : preloadedFiles) {
            fs::path file { rootDirectory / preloadedFile.first.filename() };
            AudioReaderPtr reader = createAudioReader(file, preloadedFile.first.isReverse());
            preloadedFile.second.preloadedData = readFromFile(
                *reader,
                preloadedFile.second.information.end,
                oversamplingFactor
            );
        }
    } else {
        setPreloadSize(preloadSize);
    }
}

void sfz::FilePool::triggerGarbageCollection() noexcept
{
    const std::unique_lock<SpinMutex> lastUsedLock { lastUsedMutex, std::try_to_lock };
    const std::unique_lock<SpinMutex> garbageLock { garbageMutex, std::try_to_lock };
    if (!lastUsedLock.owns_lock() || !garbageLock.owns_lock())
        return;

    const auto now = std::chrono::high_resolution_clock::now();
    swapAndPopAll(lastUsedFiles, [&](const FileId& id) {
        if (garbageToCollect.size() == garbageToCollect.capacity())
           return false;

        auto& data = preloadedFiles[id];
        if (data.status == FileData::Status::Preloaded)
            return true;

        if (data.status != FileData::Status::Done)
            return false;

        if (data.readerCount != 0)
            return false;

        const auto secondsIdle = std::chrono::duration_cast<std::chrono::seconds>(now - data.lastViewerLeftAt).count();
        if (secondsIdle < config::fileClearingPeriod)
            return false;

        data.availableFrames = 0;
        data.status = FileData::Status::Preloaded;
        garbageToCollect.push_back(std::move(data.fileData));
        return true;
    });

    std::error_code ec;
    semGarbageBarrier.post(ec);
    ASSERT(!ec);
}
