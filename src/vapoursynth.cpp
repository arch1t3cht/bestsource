//  Copyright (c) 2022-2024 Fredrik Mellbin
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to deal
//  in the Software without restriction, including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//  THE SOFTWARE.

#include "videosource.h"
#include "audiosource.h"
#include "bsshared.h"
#include "version.h"
#include <VapourSynth4.h>
#include <VSHelper4.h>
#include <vector>
#include <algorithm>
#include <memory>
#include <limits>
#include <string>
#include <chrono>
#include <mutex>

static std::once_flag BSInitOnce;

static void BSInit() {
    // Slightly ugly to avoid header inclusions
    std::call_once(BSInitOnce, []() {
#ifndef NDEBUG
        SetFFmpegLogLevel(32); // quiet
#else
        SetFFmpegLogLevel(-8); // info
#endif
        });
}

struct BestVideoSourceData {
    VSVideoInfo VI = {};
    std::unique_ptr<BestVideoSource> V;
    int64_t FPSNum;
    int64_t FPSDen;
    bool RFF;
};



static const VSFrame *VS_CC BestVideoSourceGetFrame(int n, int ActivationReason, void *InstanceData, void **, VSFrameContext *FrameCtx, VSCore *Core, const VSAPI *vsapi) {
    BestVideoSourceData *D = reinterpret_cast<BestVideoSourceData *>(InstanceData);

    if (ActivationReason == arInitial) {
        VSFrame *Dst = nullptr;
        VSFrame *AlphaDst = nullptr;
        std::unique_ptr<BestVideoFrame> Src;
        try {
            if (D->RFF) {
                Src.reset(D->V->GetFrameWithRFF(std::min(n, D->VI.numFrames - 1)));
            } else if (D->FPSNum > 0) {
                double currentTime = D->V->GetVideoProperties().StartTime +
                    (double)(std::min(n, D->VI.numFrames - 1) * D->FPSDen) / D->FPSNum;
                Src.reset(D->V->GetFrameByTime(currentTime));
            } else {
                Src.reset(D->V->GetFrame(std::min(n, D->VI.numFrames - 1)));
            }

            if (!Src)
                throw VideoException("No frame returned for frame number " + std::to_string(n) + ". This may be due to an FFmpeg bug. Delete index and retry with threads=1.");

            VSVideoFormat VideoFormat = {};
            vsapi->queryVideoFormat(&VideoFormat, Src->VF.ColorFamily, Src->VF.Float ? stFloat : stInteger, Src->VF.Bits, Src->VF.SubSamplingW, Src->VF.SubSamplingH, Core);
            VSVideoFormat AlphaFormat = {};
            vsapi->queryVideoFormat(&AlphaFormat, cfGray, VideoFormat.sampleType, VideoFormat.bitsPerSample, 0, 0, Core);

            Dst = vsapi->newVideoFrame(&VideoFormat, Src->Width, Src->Height, nullptr, Core);
            uint8_t *DstPtrs[3] = {};
            ptrdiff_t DstStride[3] = {};

            for (int Plane = 0; Plane < VideoFormat.numPlanes; Plane++) {
                DstPtrs[Plane] = vsapi->getWritePtr(Dst, Plane);
                DstStride[Plane] = vsapi->getStride(Dst, Plane);
            }

            ptrdiff_t AlphaStride = 0;
            if (Src->VF.Alpha) {
                AlphaDst = vsapi->newVideoFrame(&AlphaFormat, Src->Width, Src->Height, nullptr, Core);
                AlphaStride = vsapi->getStride(AlphaDst, 0);
                vsapi->mapSetInt(vsapi->getFramePropertiesRW(AlphaDst), "_ColorRange", 0, maAppend);
            }

            if (!Src->ExportAsPlanar(DstPtrs, DstStride, AlphaDst ? vsapi->getWritePtr(AlphaDst, 0) : nullptr, AlphaStride)) {
                throw VideoException("Cannot export to planar format for frame " + std::to_string(n));
            }

        } catch (VideoException &e) {
            vsapi->freeFrame(Dst);
            vsapi->freeFrame(AlphaDst);
            vsapi->setFilterError(("VideoSource: " + std::string(e.what())).c_str(), FrameCtx);
            return nullptr;
        }

        const VideoProperties &VP = D->V->GetVideoProperties();
        VSMap *Props = vsapi->getFramePropertiesRW(Dst);
        if (AlphaDst)
            vsapi->mapConsumeFrame(Props, "_Alpha", AlphaDst, maAppend);

        // Set AR variables
        if (VP.SAR.Num > 0 && VP.SAR.Den > 0) {
            vsapi->mapSetInt(Props, "_SARNum", VP.SAR.Num, maAppend);
            vsapi->mapSetInt(Props, "_SARDen", VP.SAR.Den, maAppend);
        }

        vsapi->mapSetInt(Props, "_Matrix", Src->Matrix, maAppend);
        vsapi->mapSetInt(Props, "_Primaries", Src->Primaries, maAppend);
        vsapi->mapSetInt(Props, "_Transfer", Src->Transfer, maAppend);
        if (Src->ChromaLocation > 0)
            vsapi->mapSetInt(Props, "_ChromaLocation", Src->ChromaLocation - 1, maAppend);

        if (Src->ColorRange == 1) // Hardcoded ffmpeg constants, nothing to see here
            vsapi->mapSetInt(Props, "_ColorRange", 1, maAppend);
        else if (Src->ColorRange == 2)
            vsapi->mapSetInt(Props, "_ColorRange", 0, maAppend);
        vsapi->mapSetData(Props, "_PictType", &Src->PictType, 1, dtUtf8, maAppend);

        // Set field information
        int FieldBased = 0;
        if (Src->InterlacedFrame)
            FieldBased = (Src->TopFieldFirst ? 2 : 1);
        vsapi->mapSetInt(Props, "_FieldBased", FieldBased, maAppend);
        vsapi->mapSetInt(Props, "RepeatField", Src->RepeatPict, maAppend);

        // FIXME, use PTS difference between frames instead?
        if (Src->Duration > 0) {
            int64_t DurNum = VP.TimeBase.Num;
            int64_t DurDen = VP.TimeBase.Den;
            vsh::muldivRational(&DurNum, &DurDen, Src->Duration, 1);
            vsapi->mapSetInt(Props, "_DurationNum", DurNum, maAppend);
            vsapi->mapSetInt(Props, "_DurationDen", DurDen, maAppend);
        }

        if (Src->HasMasteringDisplayPrimaries) {
            for (int i = 0; i < 3; i++) {
                vsapi->mapSetFloat(Props, "MasteringDisplayPrimariesX", Src->MasteringDisplayPrimaries[i][0].ToDouble(), maAppend);
                vsapi->mapSetFloat(Props, "MasteringDisplayPrimariesY", Src->MasteringDisplayPrimaries[i][1].ToDouble(), maAppend);
            }
            vsapi->mapSetFloat(Props, "MasteringDisplayWhitePointX", Src->MasteringDisplayWhitePoint[0].ToDouble(), maAppend);
            vsapi->mapSetFloat(Props, "MasteringDisplayWhitePointY", Src->MasteringDisplayWhitePoint[1].ToDouble(), maAppend);
        }

        if (Src->HasMasteringDisplayLuminance) {
            vsapi->mapSetFloat(Props, "MasteringDisplayMinLuminance", Src->MasteringDisplayMinLuminance.ToDouble(), maAppend);
            vsapi->mapSetFloat(Props, "MasteringDisplayMaxLuminance", Src->MasteringDisplayMaxLuminance.ToDouble(), maAppend);
        }

        if (Src->HasContentLightLevel) {
            vsapi->mapSetInt(Props, "ContentLightLevelMax", Src->ContentLightLevelMax, maAppend);
            vsapi->mapSetInt(Props, "ContentLightLevelAverage", Src->ContentLightLevelAverage, maAppend);
        }

        if (Src->DolbyVisionRPU && Src->DolbyVisionRPUSize) {
            vsapi->mapSetData(Props, "DolbyVisionRPU", reinterpret_cast<const char *>(Src->DolbyVisionRPU), static_cast<int>(Src->DolbyVisionRPUSize), dtBinary, maAppend);
        }

        if (Src->HDR10Plus && Src->HDR10PlusSize > 0) {
            vsapi->mapSetData(Props, "HDR10Plus", reinterpret_cast<const char *>(Src->HDR10Plus), static_cast<int>(Src->HDR10PlusSize), dtBinary, maReplace);
        }

        vsapi->mapSetInt(Props, "FlipVertical", VP.FlipVerical, maAppend);
        vsapi->mapSetInt(Props, "FlipHorizontal", VP.FlipHorizontal, maAppend);
        vsapi->mapSetInt(Props, "Rotation", VP.Rotation, maAppend);

        return Dst;
    }

    return nullptr;
}

static void VS_CC BestVideoSourceFree(void *InstanceData, VSCore *Core, const VSAPI *vsapi) {
    delete reinterpret_cast<BestVideoSourceData *>(InstanceData);
}

static void VS_CC CreateBestVideoSource(const VSMap *In, VSMap *Out, void *, VSCore *Core, const VSAPI *vsapi) {
    BSInit();

    int err;
    const char *Source = vsapi->mapGetData(In, "source", 0, nullptr);
    const char *CachePath = vsapi->mapGetData(In, "cachepath", 0, &err);
    const char *HWDevice = vsapi->mapGetData(In, "hwdevice", 0, &err);
    const char *Timecodes = vsapi->mapGetData(In, "timecodes", 0, &err);
    int Track = vsapi->mapGetIntSaturated(In, "track", 0, &err);
    if (err)
        Track = -1;
    bool VariableFormat = !!vsapi->mapGetInt(In, "variableformat", 0, &err);
    int Threads = vsapi->mapGetIntSaturated(In, "threads", 0, &err);
    bool ShowProgress = !!vsapi->mapGetInt(In, "showprogress", 0, &err);
    if (err)
        ShowProgress = true;
    int ExtraHWFrames = vsapi->mapGetIntSaturated(In, "extrahwframes", 0, &err);
    if (err)
        ExtraHWFrames = 9;
    std::map<std::string, std::string> Opts;
    if (vsapi->mapGetInt(In, "enable_drefs", 0, &err))
        Opts["enable_drefs"] = "1";
    if (vsapi->mapGetInt(In, "use_absolute_path", 0, &err))
        Opts["use_absolute_path"] = "1";

    BestVideoSourceData *D = new BestVideoSourceData();

    try {
        D->FPSNum = vsapi->mapGetInt(In, "fpsnum", 0, &err);
        if (err)
            D->FPSNum = -1;
        D->FPSDen = vsapi->mapGetInt(In, "fpsden", 0, &err);
        if (err)
            D->FPSDen = 1;
        D->RFF = !!vsapi->mapGetInt(In, "rff", 0, &err);

        if (D->FPSDen < 1)
            throw VideoException("FPS denominator needs to be 1 or greater");

        if (D->FPSNum > 0 && D->RFF)
            throw VideoException("Cannot combine CFR and RFF modes");

        if (ShowProgress) {
            auto NextUpdate = std::chrono::high_resolution_clock::now();
            int LastValue = -1;
            D->V.reset(new BestVideoSource(Source, HWDevice ? HWDevice : "", ExtraHWFrames, Track, VariableFormat, Threads, CachePath ? CachePath : "", &Opts, 
                [vsapi, Core, &NextUpdate, &LastValue](int Track, int64_t Cur, int64_t Total) {
                    if (NextUpdate < std::chrono::high_resolution_clock::now()) {
                        if (Total == INT64_MAX && Cur == Total) {
                            vsapi->logMessage(mtInformation, ("VideoSource track #" + std::to_string(Track) + " indexing complete").c_str(), Core);
                        } else {
                            int PValue = (Total > 0) ? static_cast<int>((static_cast<double>(Cur) / static_cast<double>(Total)) * 100) : static_cast<int>(Cur / (1024 * 1024));
                            if (PValue != LastValue) {
                                vsapi->logMessage(mtInformation, ("VideoSource track #" + std::to_string(Track) + " index progress " + std::to_string(PValue) + ((Total > 0) ? "%" : "MB")).c_str(), Core);
                                LastValue = PValue;
                                NextUpdate = std::chrono::high_resolution_clock::now() + std::chrono::seconds(1);
                            }
                        }
                    }
                    }));
            
        } else {
            D->V.reset(new BestVideoSource(Source, HWDevice ? HWDevice : "", ExtraHWFrames, Track, VariableFormat, Threads, CachePath ? CachePath : "", &Opts));
        }

        const VideoProperties &VP = D->V->GetVideoProperties();
        if (VP.VF.ColorFamily == 0 || !vsapi->queryVideoFormat(&D->VI.format, VP.VF.ColorFamily, VP.VF.Float, VP.VF.Bits, VP.VF.SubSamplingW, VP.VF.SubSamplingH, Core))
            throw VideoException("Unsupported video format from decoder (probably less than 8 bit or palette)");
        D->VI.width = VP.Width;
        D->VI.height = VP.Height;
        if (VariableFormat)
            D->VI = {};
        D->VI.numFrames = vsh::int64ToIntS(VP.NumFrames);
        D->VI.fpsNum = VP.FPS.Num;
        D->VI.fpsDen = VP.FPS.Den;
        vsh::reduceRational(&D->VI.fpsNum, &D->VI.fpsDen);

        if (D->FPSNum > 0) {
            vsh::reduceRational(&D->FPSNum, &D->FPSDen);
            if (VP.FPS.Den != D->FPSDen || VP.FPS.Num != D->FPSNum) {
                D->VI.fpsDen = D->FPSDen;
                D->VI.fpsNum = D->FPSNum;
                D->VI.numFrames = std::max(1, static_cast<int>((VP.Duration * D->VI.fpsNum) / D->VI.fpsDen));
            } else {
                D->FPSNum = -1;
                D->FPSDen = 1;
            }
        } else if (D->RFF) {
            D->VI.numFrames = vsh::int64ToIntS(VP.NumRFFFrames);
        }

        int SeekPreRoll = vsapi->mapGetIntSaturated(In, "seekpreroll", 0, &err);
        if (!err)
            D->V->SetSeekPreRoll(SeekPreRoll);

        if (Timecodes)
            D->V->WriteTimecodes(Timecodes);
    } catch (VideoException &e) {
        delete D;
        vsapi->mapSetError(Out, (std::string("VideoSource: ") + e.what()).c_str());
        return;
    }

    int64_t CacheSize = vsapi->mapGetInt(In, "cachesize", 0, &err);
    if (!err && CacheSize >= 0)
        D->V->SetMaxCacheSize(CacheSize * 1024 * 1024);

    vsapi->createVideoFilter(Out, "VideoSource", &D->VI, BestVideoSourceGetFrame, BestVideoSourceFree, fmUnordered, nullptr, 0, D, Core);
}

struct BestAudioSourceData {
    VSAudioInfo AI = {};
    std::unique_ptr<BestAudioSource> A;
};

static const VSFrame *VS_CC BestAudioSourceGetFrame(int n, int ActivationReason, void *InstanceData, void **, VSFrameContext *FrameCtx, VSCore *Core, const VSAPI *vsapi) {
    BestAudioSourceData *D = reinterpret_cast<BestAudioSourceData *>(InstanceData);

    if (ActivationReason == arInitial) {
        int64_t SamplesOut = std::min<int64_t>(VS_AUDIO_FRAME_SAMPLES, D->AI.numSamples - n * static_cast<int64_t>(VS_AUDIO_FRAME_SAMPLES));
        VSFrame *Dst = vsapi->newAudioFrame(&D->AI.format, static_cast<int>(SamplesOut), nullptr, Core);

        std::vector<uint8_t *> Tmp;
        Tmp.reserve(D->AI.format.numChannels);
        for (int Channel = 0; Channel < D->AI.format.numChannels; Channel++)
            Tmp.push_back(vsapi->getWritePtr(Dst, Channel));
        try {
            D->A->GetPlanarAudio(Tmp.data(), n * static_cast<int64_t>(VS_AUDIO_FRAME_SAMPLES), SamplesOut);
        } catch (AudioException &e) {
            vsapi->setFilterError(("AudioSource: " + std::string(e.what())).c_str(), FrameCtx);
            vsapi->freeFrame(Dst);
            return nullptr;
        }
        return Dst;
    }

    return nullptr;
}

static void VS_CC BestAudioSourceFree(void *InstanceData, VSCore *Core, const VSAPI *vsapi) {
    delete reinterpret_cast<BestAudioSourceData *>(InstanceData);
}

static void VS_CC CreateBestAudioSource(const VSMap *In, VSMap *Out, void *, VSCore *Core, const VSAPI *vsapi) {
    BSInit();

    int err;
    const char *Source = vsapi->mapGetData(In, "source", 0, nullptr);
    const char *CachePath = vsapi->mapGetData(In, "cachepath", 0, &err);
    int Track = vsapi->mapGetIntSaturated(In, "track", 0, &err);
    if (err)
        Track = -1;
    int AdjustDelay = vsapi->mapGetIntSaturated(In, "adjustdelay", 0, &err);
    if (err)
        AdjustDelay = -1;
    int Threads = vsapi->mapGetIntSaturated(In, "threads", 0, &err);
    bool ShowProgress = !!vsapi->mapGetInt(In, "showprogress", 0, &err);
    if (err)
        ShowProgress = true;

    std::map<std::string, std::string> Opts;
    if (vsapi->mapGetInt(In, "enable_drefs", 0, &err))
        Opts["enable_drefs"] = "1";
    if (vsapi->mapGetInt(In, "use_absolute_path", 0, &err))
        Opts["use_absolute_path"] = "1";

    double DrcScale = vsapi->mapGetFloat(In, "drc_scale", 0, &err);

    BestAudioSourceData *D = new BestAudioSourceData();

    try {
        if (ShowProgress) {
            auto NextUpdate = std::chrono::high_resolution_clock::now();
            int LastValue = -1;
            D->A.reset(new BestAudioSource(Source, Track, AdjustDelay, false, Threads, CachePath ? CachePath : "", &Opts, DrcScale,
                [vsapi, Core, &NextUpdate, &LastValue](int Track, int64_t Cur, int64_t Total) {
                    if (NextUpdate < std::chrono::high_resolution_clock::now()) {
                        if (Total == INT64_MAX && Cur == Total) {
                            vsapi->logMessage(mtInformation, ("VideoSource track #" + std::to_string(Track) + " indexing complete").c_str(), Core);
                        } else {
                            int PValue = (Total > 0) ? static_cast<int>((static_cast<double>(Cur) / static_cast<double>(Total)) * 100) : static_cast<int>(Cur / (1024 * 1024));
                            if (PValue != LastValue) {
                                vsapi->logMessage(mtInformation, ("VideoSource track #" + std::to_string(Track) + " index progress " + std::to_string(PValue) + ((Total > 0) ? "%" : "MB")).c_str(), Core);
                                LastValue = PValue;
                                NextUpdate = std::chrono::high_resolution_clock::now() + std::chrono::seconds(1);
                            }
                        }
                    }
                }));

        } else {
            D->A.reset(new BestAudioSource(Source, Track, AdjustDelay, false, Threads, CachePath ? CachePath : "", &Opts, DrcScale));
        }

        const AudioProperties &AP = D->A->GetAudioProperties();
        if (!vsapi->queryAudioFormat(&D->AI.format, AP.AF.Float, AP.AF.Bits, AP.ChannelLayout, Core))
            throw AudioException("Unsupported audio format from decoder (probably 8-bit)");
        D->AI.sampleRate = AP.SampleRate;
        D->AI.numSamples = AP.NumSamples;
        D->AI.numFrames = static_cast<int>((AP.NumSamples + VS_AUDIO_FRAME_SAMPLES - 1) / VS_AUDIO_FRAME_SAMPLES);
        if ((AP.NumSamples + VS_AUDIO_FRAME_SAMPLES - 1) / VS_AUDIO_FRAME_SAMPLES > std::numeric_limits<int>::max())
            throw AudioException("Too many audio samples, cut file into smaller parts");
    } catch (AudioException &e) {
        delete D;
        vsapi->mapSetError(Out, (std::string("AudioSource: ") + e.what()).c_str());
        return;
    }

    int64_t CacheSize = vsapi->mapGetInt(In, "cachesize", 0, &err);
    if (!err && CacheSize > 0)
        D->A->SetMaxCacheSize(CacheSize * 1024 * 1024);

    vsapi->createAudioFilter(Out, "AudioSource", &D->AI, BestAudioSourceGetFrame, BestAudioSourceFree, fmUnordered, nullptr, 0, D, Core);
}

static void VS_CC SetDebugOutput(const VSMap *in, VSMap *out, void *, VSCore *, const VSAPI *vsapi) {
    BSInit();
    SetBSDebugOutput(!!vsapi->mapGetInt(in, "enable", 0, nullptr));
}

static void VS_CC SetLogLevel(const VSMap *in, VSMap *out, void *, VSCore *, const VSAPI *vsapi) {
    BSInit();
    int err;
    int level = vsapi->mapGetIntSaturated(in, "level", 0, &err);
    if (err)
        level = 32;
    vsapi->mapSetInt(out, "level", SetFFmpegLogLevel(level), maReplace);
}

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin *plugin, const VSPLUGINAPI *vspapi) {
    vspapi->configPlugin("com.vapoursynth.bestsource", "bs", "Best Source 2", VS_MAKE_VERSION(BEST_SOURCE_VERSION_MAJOR, BEST_SOURCE_VERSION_MINOR), VS_MAKE_VERSION(VAPOURSYNTH_API_MAJOR, 0), 0, plugin);
    vspapi->registerFunction("VideoSource", "source:data;track:int:opt;variableformat:int:opt;fpsnum:int:opt;fpsden:int:opt;rff:int:opt;threads:int:opt;seekpreroll:int:opt;enable_drefs:int:opt;use_absolute_path:int:opt;cachepath:data:opt;cachesize:int:opt;hwdevice:data:opt;extrahwframes:int:opt;timecodes:data:opt;showprogress:int:opt;", "clip:vnode;", CreateBestVideoSource, nullptr, plugin);
    vspapi->registerFunction("AudioSource", "source:data;track:int:opt;adjustdelay:int:opt;threads:int:opt;enable_drefs:int:opt;use_absolute_path:int:opt;drc_scale:float:opt;cachepath:data:opt;cachesize:int:opt;showprogress:int:opt;", "clip:anode;", CreateBestAudioSource, nullptr, plugin);
    vspapi->registerFunction("SetDebugOutput", "enable:int;", "", SetDebugOutput, nullptr, plugin);
    vspapi->registerFunction("SetFFmpegLogLevel", "level:int;", "level:int;", SetLogLevel, nullptr, plugin);
}
