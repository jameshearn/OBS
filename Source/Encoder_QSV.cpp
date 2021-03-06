/********************************************************************************
 Copyright (C) 2013 Ruwen Hahn <palana@stunned.de>
 
 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
********************************************************************************/


#include "Main.h"


#include <inttypes.h>
#include <ws2tcpip.h>

#include "mfxstructures.h"
#include "mfxmvc.h"
#include "mfxvideo.h"
#include "mfxvideo++.h"
#include "mfxplugin++.h"

#include <algorithm>
#include <memory>

#include "../QSVHelper/IPCInfo.h"
#include "../QSVHelper/WindowsStuff.h"

extern "C"
{
#include "../x264/x264.h"
}

namespace
{
#define TO_STR(a) TEXT(#a)

    const float baseCRF = 22.0f;
    const struct impl_parameters
    {
        mfxU32 type,
               intf;
        mfxVersion version;
    };
    const TCHAR* implStr[] = {
        TO_STR(MFX_IMPL_AUTO),
        TO_STR(MFX_IMPL_SOFTWARE),
        TO_STR(MFX_IMPL_HARDWARE),
        TO_STR(MFX_IMPL_AUTO_ANY),
        TO_STR(MFX_IMPL_HARDWARE_ANY),
        TO_STR(MFX_IMPL_HARDWARE2),
        TO_STR(MFX_IMPL_HARDWARE3),
        TO_STR(MFX_IMPL_HARDWARE4),
        TEXT("MFX_IMPL_UNKNOWN")
    };
    const TCHAR* usageStr[] = {
        TO_STR(MFX_TARGETUSAGE_UNKNOWN),
        TO_STR(MFX_TARGETUSAGE_BEST_QUALITY),
        TO_STR(2),
        TO_STR(3),
        TO_STR(MFX_TARGETUSAGE_BALANCED),
        TO_STR(5),
        TO_STR(6),
        TO_STR(MFX_TARGETUSAGE_BEST_SPEED)
    };

    CTSTR qsv_intf_str(const mfxU32 impl)
    {
        switch(impl & (-MFX_IMPL_VIA_ANY))
        {
#define VIA_STR(x) case MFX_IMPL_VIA_##x: return TEXT(" | ") TO_STR(MFX_IMPL_VIA_##x)
            VIA_STR(ANY);
            VIA_STR(D3D9);
            VIA_STR(D3D11);
#undef VIA_STR
        default: return TEXT("");
        }
    };

#define MFX_TIME_FACTOR 90
    template<class T>
    auto timestampFromMS(T t) -> decltype(t*MFX_TIME_FACTOR)
    {
        return t*MFX_TIME_FACTOR;
    }

    template<class T>
    auto msFromTimestamp(T t) -> decltype(t/MFX_TIME_FACTOR)
    {
        return t/MFX_TIME_FACTOR;
    }
#undef MFX_TIME_FACTOR

    template <class T>
    void zero(T& t, size_t size=sizeof(T))
    {
        memset(&t, 0, size);
    }


    bool spawn_helper(String &event_prefix, safe_handle &qsvhelper_process, safe_handle &qsvhelper_thread, IPCWaiter &process_waiter)
    {
        String helper_path;
        auto dir_size = GetCurrentDirectory(0, NULL);
        helper_path.SetLength(dir_size);
        GetCurrentDirectory(dir_size, helper_path);

        helper_path << "/";

        String helper = helper_path;
        String helper_name = "QSVHelper.exe";
        helper << helper_name;
        
        PROCESS_INFORMATION pi;
        STARTUPINFO si;

        zero(pi);
        zero(si);
        si.cb = sizeof(si);

        if(!CreateProcess(helper, helper_name+" "+OBSGetAppDataPath(), nullptr, nullptr, false, 0, nullptr, helper_path, &si, &pi))
            return false;

        qsvhelper_process.reset(pi.hProcess);
        qsvhelper_thread.reset(pi.hThread);
        process_waiter.push_back(pi.hProcess);
        process_waiter.push_back(pi.hThread);

        event_prefix = FormattedString(TEXT("%s%u"), helper_name.Array(), pi.dwProcessId);

        return true;
    }

    enum qsv_cpu_platform
    {
        QSV_CPU_PLATFORM_UNKNOWN,
        QSV_CPU_PLATFORM_SNB,
        QSV_CPU_PLATFORM_IVB,
        QSV_CPU_PLATFORM_HSW
    };

    qsv_cpu_platform qsv_get_cpu_platform()
    {
        int cpuInfo[4];
        __cpuid(cpuInfo, 1);
        BYTE model = ((cpuInfo[0] >> 4) & 0xF) + ((cpuInfo[0] >> 12) & 0xF0);
        BYTE family = ((cpuInfo[0] >> 8) & 0xF) + ((cpuInfo[0] >> 20) & 0xFF);

        // See Intel 64 and IA-32 Architectures Software Developer's Manual, Vol 3C Table 35-1
        if (family != 6)
            return QSV_CPU_PLATFORM_UNKNOWN;

        switch (model)
        {
        case 0x2a:
        case 0x2d:
            return QSV_CPU_PLATFORM_SNB;

        case 0x3a:
        case 0x3e:
            return QSV_CPU_PLATFORM_IVB;

        case 0x3c:
        case 0x45:
        case 0x46:
            return QSV_CPU_PLATFORM_HSW;
        }

        return QSV_CPU_PLATFORM_UNKNOWN;
    }

    struct DTSGenerator
    {
        bool ver_1_6;
        bool bframes_pyramid;
        unsigned bframe_delay;
        bool use_bs_dts;

        uint64_t frame_ticks;

        uint64_t frames_out;

        List<mfxI64> dts;

        List<mfxI64> init_pts;

        DTSGenerator() : ver_1_6(false), bframes_pyramid(false), bframe_delay(0), use_bs_dts(false) {}

        void Init(unsigned bframe_delay_, mfxVersion &ver, bool use_cfr, uint64_t frame_ticks_)
        {
            bframe_delay = bframe_delay_;
            ver_1_6 = (ver.Major >= 1 && ver.Minor >= 6);
            bframes_pyramid = qsv_get_cpu_platform() >= QSV_CPU_PLATFORM_HSW;
            use_bs_dts = ver_1_6 && use_cfr;
            frame_ticks = frame_ticks_;
        }

        void add(uint64_t ts)
        {
            if (use_bs_dts)
                return;

            if (init_pts.Num() || frames_out == 0)
                init_pts << ts;
            dts << ts;
        }

        int64_t operator()(uint64_t bs_pts, int64_t bs_dts)
        {
            if (use_bs_dts)
                return bs_dts;

            int64_t result = bs_dts;

            if (frames_out == 0 && ver_1_6 && bframes_pyramid)
            {
                int delay = (int)((bs_pts - bs_dts + frame_ticks / 2) / frame_ticks);
                if (delay > 0)
                    bframe_delay = delay;
                Log(L"Recalculated bframe_delay: %u, init_pts.Num: %u", bframe_delay, init_pts.Num());
            }

            if (frames_out <= bframe_delay)
            {
                if (bframe_delay >= init_pts.Num())
                {
                    AppWarning(L"bframe_delay=%u >= init_pts.Num=%u", bframe_delay, init_pts.Num());
                    bframe_delay = init_pts.Num() - 1;
                }
                result = init_pts[(unsigned)frames_out] - init_pts[bframe_delay];
            }
            else
            {
                init_pts.Clear();
                result = dts[0];
                dts.Remove(0);
            }
            //Log(L"bs_dts=%u dts.Num=%u frames_out=%u bframe_delay=%u result=%lli bs_pts=%llu bs_dts=%lli", use_bs_dts, dts.Num(), frames_out, bframe_delay, result, bs_pts, bs_dts);

            frames_out += 1;
            return result;
        }
    };
}

bool CheckQSVHardwareSupport(bool log=true)
{
    safe_handle helper_process, helper_thread;
    IPCWaiter waiter;
    String event_prefix;
    if(!spawn_helper(event_prefix, helper_process, helper_thread, waiter))
        return false;

    ipc_init_request req((event_prefix + INIT_REQUEST).Array());
    req->mode = req->MODE_QUERY;
    req.signal();

    if(!waiter.wait(INFINITE))
        return false;

    DWORD code = 0;
    if(!GetExitCodeProcess(helper_process.h, &code))
        return false;

    if(code == 0)
    {
        if(log)
            Log(TEXT("Found QSV hardware support"));
        return true;
    }

    if(log)
        Log(TEXT("Failed to initialize QSV hardware session"));
    return false;
}

struct VideoPacket
{
    List<BYTE> Packet;
    inline void FreeData() {Packet.Clear();}
};

class QSVEncoder : public VideoEncoder
{
    mfxVersion ver;
    bool bHaveCustomImpl;

    safe_handle qsvhelper_process,
                qsvhelper_thread;
    IPCSignal stop;

    ipc_bitstream_buff bs_buff;
    ipc_bitstream_info bs_info;
    struct encode_task
    {
        mfxFrameSurface1 surf;
        mfxBitstream bs;
        mfxFrameData *frame;
    };
    List<encode_task> encode_tasks;

    CircularList<unsigned> queued_tasks,
                           idle_tasks;

    IPCLockedSignalledArray<queued_frame> frame_queue;

    ipc_frame_buff frame_buff;
    ipc_frame_buff_status frame_buff_status;
    ipc_filled_bitstream filled_bitstream;

    IPCWaiter process_waiter,
              filled_bitstream_waiter;

    List<mfxFrameData> frames;

    uint64_t out_frames;
    DTSGenerator dts_gen;

    mfxU16 target_usage,
           max_bitrate;

    String event_prefix;

    int fps;

    bool bRequestKeyframe,
         bFirstFrameProcessed;

    UINT width, height;

    bool bUseCBR, bUseCFR;

    List<VideoPacket> CurrentPackets;
    List<BYTE> HeaderPacket, SEIData;

    INT64 delayOffset;

    int frameShift;

    inline void ClearPackets()
    {
        for(UINT i=0; i<CurrentPackets.Num(); i++)
            CurrentPackets[i].FreeData();
        CurrentPackets.Clear();
    }

public:

    QSVEncoder(int fps, int width, int height, int quality, CTSTR preset, bool bUse444, ColorDescription &colorDesc, int maxBitrate, int bufferSize, bool bUseCFR_)
        : fps(fps), bFirstFrameProcessed(false), width(width), height(height), max_bitrate(maxBitrate)
    {
        bUseCBR = AppConfig->GetInt(TEXT("Video Encoding"), TEXT("UseCBR")) != 0;
        bUseCFR = bUseCFR_;

        UINT keyframeInterval = AppConfig->GetInt(TEXT("Video Encoding"), TEXT("KeyframeInterval"), 6);

        int keyint = fps*keyframeInterval;
        int bframes = 7;

        bool main_profile = (AppConfig->GetString(TEXT("Video Encoding"), TEXT("X264Profile"), TEXT("high")) != L"high") ? true : false;

        bHaveCustomImpl = false;
        impl_parameters custom = { 0 };

        BOOL bUseCustomParams = AppConfig->GetInt(TEXT("Video Encoding"), TEXT("UseCustomSettings"))
                             && AppConfig->GetInt(TEXT("Video Encoding"), TEXT("QSVUseVideoEncoderSettings"));
        if(bUseCustomParams)
        {
            StringList paramList;
            String strCustomParams = AppConfig->GetString(TEXT("Video Encoding"), TEXT("CustomSettings"));
            strCustomParams.KillSpaces();

            if(strCustomParams.IsValid())
            {
                Log(TEXT("Using custom encoder settings: \"%s\""), strCustomParams.Array());

                strCustomParams.GetTokenList(paramList, ' ', FALSE);
                for(UINT i=0; i<paramList.Num(); i++)
                {
                    String &strParam = paramList[i];
                    if(!schr(strParam, '='))
                        continue;

                    String strParamName = strParam.GetToken(0, '=');
                    String strParamVal  = strParam.GetTokenOffset(1, '=');

                    if(strParamName == "keyint")
                    {
                        int keyint_ = strParamVal.ToInt();
                        if(keyint_ < 0)
                            continue;
                        keyint = keyint_;
                    }
                    else if(strParamName == "bframes")
                    {
                        int bframes_ = strParamVal.ToInt();
                        if(bframes_ < 0)
                            continue;
                        bframes = bframes_;
                    }
                    else if(strParamName == "qsvimpl")
                    {
                        StringList bits;
                        strParamVal.GetTokenList(bits, ',', true);
                        if(bits.Num() < 3)
                            continue;

                        StringList version;
                        bits[2].GetTokenList(version, '.', false);
                        if(version.Num() != 2)
                            continue;

                        custom.type = bits[0].ToInt();
                        if(custom.type == 0)
                            custom.type = MFX_IMPL_HARDWARE_ANY;

                        auto &intf = bits[1].MakeLower();
                        custom.intf = intf == "d3d11" ? MFX_IMPL_VIA_D3D11 : (intf == "d3d9" ? MFX_IMPL_VIA_D3D9 : MFX_IMPL_VIA_ANY);

                        custom.version.Major = version[0].ToInt();
                        custom.version.Minor = version[1].ToInt();
                        bHaveCustomImpl = true;
                    }
                }
            }
        }

        if(!spawn_helper(event_prefix, qsvhelper_process, qsvhelper_thread, process_waiter))
            CrashError(TEXT("Couldn't launch QSVHelper: %u"), GetLastError()); //FIXME: convert to localized error

        ipc_init_request request((event_prefix + INIT_REQUEST).Array());

        request->mode = request->MODE_ENCODE;
        request->obs_process_id = GetCurrentProcessId();

        request->fps = fps;
        request->keyint = keyint;
        request->bframes = bframes;
        request->width = width;
        request->height = height;
        request->max_bitrate = maxBitrate;
        request->buffer_size = bufferSize;
        request->use_cbr = bUseCBR;
        request->main_profile = main_profile;
        request->full_range = colorDesc.fullRange;
        request->matrix = colorDesc.matrix;
        request->primaries = colorDesc.primaries;
        request->transfer = colorDesc.transfer;
        request->use_custom_impl = bHaveCustomImpl;
        request->custom_impl = custom.type;
        request->custom_intf = custom.intf;
        request->custom_version = custom.version;

        request.signal();
        
        ipc_init_response response((event_prefix + INIT_RESPONSE).Array());
        IPCWaiter response_waiter = process_waiter;
        response_waiter.push_back(response.signal_);
        if(response_waiter.wait_for_two(0, 1, INFINITE))
        {
            DWORD code = 0;
            if(!GetExitCodeProcess(qsvhelper_process.h, &code))
                CrashError(TEXT("Failed to initialize QSV session.")); //FIXME: convert to localized error
            switch(code)
            {
            case EXIT_INCOMPATIBLE_CONFIGURATION:
                if (bHaveCustomImpl)
                    CrashError(TEXT("QSVHelper.exe has exited because of an incompatible qsvimpl custom parameter (before response)"));
                else
                    CrashError(TEXT("QSVHelper.exe has exited because the encoder was not initialized"));
            case EXIT_NO_VALID_CONFIGURATION:
                if(OSGetVersion() < 8)
                    CrashError(TEXT("QSVHelper.exe could not find a valid configuration. Make sure you have a (virtual) display connected to your iGPU")); //FIXME: convert to localized error
                CrashError(TEXT("QSVHelper.exe could not find a valid configuration"));
            default:
                CrashError(TEXT("QSVHelper.exe has exited with code %i (before response)"), code); //FIXME: convert to localized error
            }
        }

        Log(TEXT("------------------------------------------"));

        if(bHaveCustomImpl && !response->using_custom_impl)
            AppWarning(TEXT("Could not initialize QSV session using custom settings")); //FIXME: convert to localized error

        ver = response->version;
        auto intf_str = qsv_intf_str(response->requested_impl),
             actual_intf_str = qsv_intf_str(response->actual_impl);
        auto length = std::distance(std::begin(implStr), std::end(implStr));
        auto impl = response->requested_impl & (MFX_IMPL_VIA_ANY - 1);
        if(impl > length) impl = static_cast<mfxIMPL>(length-1);
        Log(TEXT("QSV version %u.%u using %s%s (actual: %s%s)"), ver.Major, ver.Minor,
            implStr[impl], intf_str, implStr[response->actual_impl & (MFX_IMPL_VIA_ANY - 1)], actual_intf_str);

        target_usage = response->target_usage;

        encode_tasks.SetSize(response->bitstream_num);

        bs_buff = ipc_bitstream_buff((event_prefix + BITSTREAM_BUFF).Array(), response->bitstream_size*response->bitstream_num);

        if(!bs_buff)
            CrashError(TEXT("Failed to initialize QSV bitstream buffer (%u)"), GetLastError());

        mfxU8 *bs_start = (mfxU8*)(((size_t)&bs_buff + 31)/32*32);
        for(unsigned i = 0; i < encode_tasks.Num(); i++)
        {
            zero(encode_tasks[i].surf);

            mfxBitstream &bs = encode_tasks[i].bs;
            zero(bs);
            bs.Data = bs_start + i*response->bitstream_size;
            bs.MaxLength = response->bitstream_size;

            idle_tasks << i;
        }

        frames.SetSize(response->frame_num);

        frame_buff = ipc_frame_buff((event_prefix + FRAME_BUFF).Array(), response->frame_size*response->frame_num);

        if(!frame_buff)
            CrashError(TEXT("Failed to initialize QSV frame buffer (%u)"), GetLastError());

        mfxU8 *frame_start = (mfxU8*)(((size_t)&frame_buff + 15)/16*16);
        for(unsigned i = 0; i < frames.Num(); i++)
        {
            mfxFrameData& frame = frames[i];
            zero(frame);
            frame.Y = frame_start + i * response->frame_size;
            frame.UV = frame_start + i * response->frame_size + response->UV_offset;
            frame.V = frame_start + i * response->frame_size + response->V_offset;
            frame.Pitch = response->frame_pitch;
        }

        Log(TEXT("Using %u bitstreams and %u frame buffers"), encode_tasks.Num(), frames.Num());

        Log(TEXT("------------------------------------------"));
        Log(GetInfoString());
        Log(TEXT("------------------------------------------"));

        DataPacket packet;
        GetHeaders(packet);

        frame_queue = ipc_frame_queue((event_prefix + FRAME_QUEUE).Array(), frames.Num());
        if(!frame_queue)
            CrashError(TEXT("Failed to initialize frame queue (%u)"), GetLastError());

        frame_buff_status = ipc_frame_buff_status((event_prefix + FRAME_BUFF_STATUS).Array(), frames.Num());
        if(!frame_buff_status)
            CrashError(TEXT("Failed to initialize QSV frame buffer status (%u)"), GetLastError());

        bs_info = ipc_bitstream_info((event_prefix + BITSTREAM_INFO).Array(), response->bitstream_num);
        if(!bs_info)
            CrashError(TEXT("Failed to initialize QSV bitstream info (%u)"), GetLastError());

        filled_bitstream = ipc_filled_bitstream((event_prefix + FILLED_BITSTREAM).Array());
        if(!filled_bitstream)
            CrashError(TEXT("Failed to initialize bitstream signal (%u)"), GetLastError());

        stop = ipc_stop((event_prefix + STOP_REQUEST).Array());
        if(!stop)
            CrashError(TEXT("Failed to initialize QSV stop signal (%u)"), GetLastError());

        filled_bitstream_waiter = process_waiter;
        filled_bitstream_waiter.push_back(filled_bitstream.signal_);

        dts_gen.Init(response->bframe_delay, ver, bUseCFR_, response->frame_ticks);
    }

    ~QSVEncoder()
    {
        stop.signal();
        ClearPackets();
    }

#ifndef SEI_USER_DATA_UNREGISTERED
#define SEI_USER_DATA_UNREGISTERED 0x5
#endif

    void ProcessEncodedFrame(List<DataPacket> &packets, List<PacketType> &packetTypes, DWORD outputTimestamp, mfxU32 wait=0)
    {
        if(!filled_bitstream_waiter.wait_for(2, wait))
            return;

        uint32_t index = 0;
        {
            auto lock = lock_mutex(filled_bitstream);
            index = *filled_bitstream;
            *filled_bitstream = -1;
        }
        encode_task& task = encode_tasks[index];

        mfxBitstream& bs = task.bs;

        List<x264_nal_t> nalOut;
        mfxU8 *start, *end;
        {
            bitstream_info &info = bs_info[index];
            bs.TimeStamp = info.time_stamp;
            bs.DataLength = info.data_length;
            bs.DataOffset = info.data_offset;
            bs.PicStruct = info.pic_struct;
            bs.FrameType = info.frame_type;
            bs.DecodeTimeStamp = dts_gen(info.time_stamp, info.decode_time_stamp);
        }
        start = bs.Data + bs.DataOffset;
        end = bs.Data + bs.DataOffset + bs.DataLength;
        const static mfxU8 start_seq[] = {0, 0, 1};
        start = std::search(start, end, start_seq, start_seq+3);
        while(start != end)
        {
            decltype(start) next = std::search(start+1, end, start_seq, start_seq+3);
            x264_nal_t nal;
            nal.i_ref_idc = start[3]>>5;
            nal.i_type = start[3]&0x1f;
            if(nal.i_type == NAL_SLICE_IDR)
                nal.i_ref_idc = NAL_PRIORITY_HIGHEST;
            else if(nal.i_type == NAL_SLICE)
            {
                switch(bs.FrameType & (MFX_FRAMETYPE_REF | (MFX_FRAMETYPE_S-1)))
                {
                case MFX_FRAMETYPE_REF|MFX_FRAMETYPE_I:
                case MFX_FRAMETYPE_REF|MFX_FRAMETYPE_P:
                    nal.i_ref_idc = NAL_PRIORITY_HIGH;
                    break;
                case MFX_FRAMETYPE_REF|MFX_FRAMETYPE_B:
                    nal.i_ref_idc = NAL_PRIORITY_LOW;
                    break;
                case MFX_FRAMETYPE_B:
                    nal.i_ref_idc = NAL_PRIORITY_DISPOSABLE;
                    break;
                default:
                    Log(TEXT("Unhandled frametype %u"), bs.FrameType);
                }
            }
            start[3] = ((nal.i_ref_idc<<5)&0x60) | nal.i_type;
            nal.p_payload = start;
            nal.i_payload = int(next-start);
            nalOut << nal;
            start = next;
        }
        size_t nalNum = nalOut.Num();

        packets.Clear();
        ClearPackets();

        INT64 dts = msFromTimestamp(bs.DecodeTimeStamp);

        INT64 in_pts = msFromTimestamp(task.surf.Data.TimeStamp),
              out_pts = msFromTimestamp(bs.TimeStamp);

        if(!bFirstFrameProcessed && nalNum)
        {
            delayOffset = -dts;
            bFirstFrameProcessed = true;
        }

        INT64 ts = INT64(outputTimestamp);
        int timeOffset;

        //if frame duplication is being used, the shift will be insignificant, so just don't bother adjusting audio
        timeOffset = int(out_pts-dts);
        timeOffset += frameShift;

        if(nalNum && timeOffset < 0)
        {
            frameShift -= timeOffset;
            timeOffset = 0;
        }
        //Log(TEXT("inpts: %005d, dts: %005d, pts: %005d, timestamp: %005d, offset: %005d, newoffset: %005d"), task.surf.Data.TimeStamp/90, dts, bs.TimeStamp/90, outputTimestamp, timeOffset, bs.TimeStamp/90-dts);

        timeOffset = htonl(timeOffset);

        BYTE *timeOffsetAddr = ((BYTE*)&timeOffset)+1;

        VideoPacket *newPacket = NULL;

        PacketType bestType = PacketType_VideoDisposable;
        bool bFoundFrame = false;

        for(unsigned i=0; i<nalNum; i++)
        {
            x264_nal_t &nal = nalOut[i];

            if(nal.i_type == NAL_SEI)
            {
                BYTE *end = nal.p_payload + nal.i_payload;
                BYTE *skip = nal.p_payload;
                while(*(skip++) != 0x1);
                int skipBytes = (int)(skip-nal.p_payload);

                int newPayloadSize = (nal.i_payload-skipBytes);
                BYTE *sei_start = skip+1;
                while(sei_start < end)
                {
                    BYTE *sei = sei_start;
                    int sei_type = 0;
                    while(*sei == 0xff)
                    {
                        sei_type += 0xff;
                        sei += 1;
                    }
                    sei_type += *sei++;

                    int payload_size = 0;
                    while(*sei == 0xff)
                    {
                        payload_size += 0xff;
                        sei += 1;
                    }
                    payload_size += *sei++;

                    const static BYTE emulation_prevention_pattern[] = {0, 0, 3};
                    BYTE *search = sei;
                    for(BYTE *search = sei;;)
                    {
                        search = std::search(search, sei+payload_size, emulation_prevention_pattern, emulation_prevention_pattern+3);
                        if(search == sei+payload_size)
                            break;
                        payload_size += 1;
                        search += 3;
                    }

                    int sei_size = (int)(sei-sei_start) + payload_size;
                    sei_start[-1] = NAL_SEI;

                    if(sei_type == SEI_USER_DATA_UNREGISTERED) {
                        SEIData.Clear();
                        BufferOutputSerializer packetOut(SEIData);

                        packetOut.OutputDword(htonl(sei_size + 2));
                        packetOut.Serialize(sei_start - 1, sei_size + 1);
                        packetOut.OutputByte(0x80);
                    } else {
                        if (!newPacket)
                            newPacket = CurrentPackets.CreateNew();

                        BufferOutputSerializer packetOut(newPacket->Packet);

                        packetOut.OutputDword(htonl(sei_size + 2));
                        packetOut.Serialize(sei_start - 1, sei_size + 1);
                        packetOut.OutputByte(0x80);
                    }
                    sei_start += sei_size;

                    if (*sei_start == 0x80 && std::find_if_not(sei_start + 1, end, [](uint8_t val) { return val == 0; }) == end) //find rbsp_trailing_bits
                        break;
                }
            }
            else if(nal.i_type == NAL_AUD)
            {
                BYTE *skip = nal.p_payload;
                while(*(skip++) != 0x1);
                int skipBytes = (int)(skip-nal.p_payload);

                int newPayloadSize = (nal.i_payload-skipBytes);

                if (!newPacket)
                    newPacket = CurrentPackets.CreateNew();

                BufferOutputSerializer packetOut(newPacket->Packet);

                packetOut.OutputDword(htonl(newPayloadSize));
                packetOut.Serialize(nal.p_payload+skipBytes, newPayloadSize);
            }
            else if(nal.i_type == NAL_SLICE_IDR || nal.i_type == NAL_SLICE)
            {
                BYTE *skip = nal.p_payload;
                while(*(skip++) != 0x1);
                int skipBytes = (int)(skip-nal.p_payload);

                if (!newPacket)
                    newPacket = CurrentPackets.CreateNew();

                if (!bFoundFrame)
                {
                    newPacket->Packet.Insert(0, (nal.i_type == NAL_SLICE_IDR) ? 0x17 : 0x27);
                    newPacket->Packet.Insert(1, 1);
                    newPacket->Packet.InsertArray(2, timeOffsetAddr, 3);

                    bFoundFrame = true;
                }

                int newPayloadSize = (nal.i_payload-skipBytes);
                BufferOutputSerializer packetOut(newPacket->Packet);

                packetOut.OutputDword(htonl(newPayloadSize));
                packetOut.Serialize(nal.p_payload+skipBytes, newPayloadSize);

                switch(nal.i_ref_idc)
                {
                case NAL_PRIORITY_DISPOSABLE:   bestType = MAX(bestType, PacketType_VideoDisposable);  break;
                case NAL_PRIORITY_LOW:          bestType = MAX(bestType, PacketType_VideoLow);         break;
                case NAL_PRIORITY_HIGH:         bestType = MAX(bestType, PacketType_VideoHigh);        break;
                case NAL_PRIORITY_HIGHEST:      bestType = MAX(bestType, PacketType_VideoHighest);     break;
                }
            }
            /*else if(nal.i_type == NAL_SPS)
            {
            VideoPacket *newPacket = CurrentPackets.CreateNew();
            BufferOutputSerializer headerOut(newPacket->Packet);

            headerOut.OutputByte(0x17);
            headerOut.OutputByte(0);
            headerOut.Serialize(timeOffsetAddr, 3);
            headerOut.OutputByte(1);
            headerOut.Serialize(nal.p_payload+5, 3);
            headerOut.OutputByte(0xff);
            headerOut.OutputByte(0xe1);
            headerOut.OutputWord(htons(nal.i_payload-4));
            headerOut.Serialize(nal.p_payload+4, nal.i_payload-4);

            x264_nal_t &pps = nalOut[i+1]; //the PPS always comes after the SPS

            headerOut.OutputByte(1);
            headerOut.OutputWord(htons(pps.i_payload-4));
            headerOut.Serialize(pps.p_payload+4, pps.i_payload-4);
            }*/
            else
                continue;
        }

        packetTypes << bestType;

        packets.SetSize(CurrentPackets.Num());
        for(UINT i=0; i<packets.Num(); i++)
        {
            packets[i].lpPacket = CurrentPackets[i].Packet.Array();
            packets[i].size     = CurrentPackets[i].Packet.Num();
        }

        idle_tasks << index;
        assert(queued_tasks[0] == index);
        queued_tasks.Remove(0);
    }

    virtual void RequestBuffers(LPVOID buffers)
    {
        if(!buffers)
            return;

        mfxFrameData& buff = *(mfxFrameData*)buffers;

        auto lock = lock_mutex(frame_buff_status);

        if(buff.MemId && !frame_buff_status[(unsigned)buff.MemId-1]) //Reuse buffer if not in use
            return;

        for(unsigned i = 0; i < frames.Num(); i++)
        {
            if(frame_buff_status[i] || frames[i].MemId)
                continue;
            mfxFrameData& data = frames[i];
            buff.Y = data.Y;
            buff.UV = data.UV;
            buff.Pitch = data.Pitch;
            if(buff.MemId)
                frames[(unsigned)buff.MemId-1].MemId = nullptr;
            buff.MemId = mfxMemId(i+1);
            data.MemId = mfxMemId(i+1);
            return;
        }
        Log(TEXT("Error: all frames are in use"));
    }

    void QueueEncodeTask(mfxFrameSurface1 *pic)
    {
        if (!pic)
            return;

        profileSegment("QueueEncodeTask");
        encode_task& task = encode_tasks[idle_tasks[0]];

        auto lock_queue = lock_mutex(frame_queue);

        for(unsigned i = 0; i < frame_queue.size; i++)
        {
            queued_frame &info = frame_queue[i];

            if(info.is_new)
                continue;
            queued_tasks << idle_tasks[0];
            idle_tasks.Remove(0);
            info.is_new = true;

            info.request_keyframe = bRequestKeyframe;
            bRequestKeyframe = false;

            info.timestamp = task.surf.Data.TimeStamp = timestampFromMS(pic->Data.TimeStamp);
            dts_gen.add(info.timestamp);
            info.frame_index = (uint32_t)pic->Data.MemId-1;
            auto lock_status = lock_mutex(frame_buff_status);
            frame_buff_status[info.frame_index] += 1;
            frame_queue.signal();
            return;
        }
        CrashError(TEXT("QSV encoder is too slow"));
    }

    bool Encode(LPVOID picInPtr, List<DataPacket> &packets, List<PacketType> &packetTypes, DWORD outputTimestamp)
    {
        if(!process_waiter.wait_timeout())
        {
            int code = 0;
            if(!GetExitCodeProcess(process_waiter.list[0], (LPDWORD)&code))
                CrashError(TEXT("QSVHelper.exe exited!"));
            switch(code)
            {
            case EXIT_INCOMPATIBLE_CONFIGURATION:
                if (bHaveCustomImpl)
                    CrashError(TEXT("QSVHelper.exe has exited because of an incompatible qsvimpl custom parameter"));
                else
                    CrashError(TEXT("QSVHelper.exe has exited because the encoder was not initialized"));
            default:
                CrashError(TEXT("QSVHelper.exe has exited with code %i"), code);
            }
        }

        bool queued = false;
        if (idle_tasks.Num())
        {
            QueueEncodeTask((mfxFrameSurface1*)picInPtr);
            queued = true;
        }

        profileIn("ProcessEncodedFrame");
        do
        {
            ProcessEncodedFrame(packets, packetTypes, outputTimestamp, idle_tasks.Num() ? 0 : INFINITE);
        }
        while(!idle_tasks.Num());
        profileOut;

        if(!queued)
            QueueEncodeTask((mfxFrameSurface1*)picInPtr);

        return true;
    }

    void GetHeaders(DataPacket &packet)
    {
        if(!HeaderPacket.Num())
        {
            IPCSignalledType<spspps_size> spspps((event_prefix + SPSPPS_SIZES).Array());
            IPCSignalledArray<mfxU8> sps((event_prefix + SPS_BUFF).Array(), spspps->sps_size),
                                     pps((event_prefix + PPS_BUFF).Array(), spspps->pps_size);

            IPCWaiter spspps_waiter = process_waiter;
            spspps_waiter.push_back(spspps.signal_);

            if(!spspps_waiter.wait_for(2, INFINITE))
                return;

            BufferOutputSerializer headerOut(HeaderPacket);

            headerOut.OutputByte(0x17);
            headerOut.OutputByte(0);
            headerOut.OutputByte(0);
            headerOut.OutputByte(0);
            headerOut.OutputByte(0);
            headerOut.OutputByte(1);
            headerOut.Serialize(sps+5, 3);
            headerOut.OutputByte(0xff);
            headerOut.OutputByte(0xe1);
            headerOut.OutputWord(htons(spspps->sps_size-4));
            headerOut.Serialize(sps+4, spspps->sps_size-4);


            headerOut.OutputByte(1);
            headerOut.OutputWord(htons(spspps->pps_size-4));
            headerOut.Serialize(pps+4, spspps->pps_size-4);
        }

        packet.lpPacket = HeaderPacket.Array();
        packet.size     = HeaderPacket.Num();
    }

    virtual void GetSEI(DataPacket &packet)
    {
        packet.lpPacket = SEIData.Array();
        packet.size     = SEIData.Num();
    }

    int GetBitRate() const
    {
        return max_bitrate;
    }

    String GetInfoString() const
    {
        String strInfo;

        strInfo << TEXT("Video Encoding: QSV")    <<
                   TEXT("\r\n    fps: ")          << IntString(fps) <<
                   TEXT("\r\n    width: ")        << IntString(width) << TEXT(", height: ") << IntString(height) <<
                   TEXT("\r\n    target-usage: ") << usageStr[target_usage] <<
                   TEXT("\r\n    CBR: ")          << CTSTR((bUseCBR) ? TEXT("yes") : TEXT("no")) <<
                   TEXT("\r\n    CFR: ")          << CTSTR((bUseCFR) ? TEXT("yes") : TEXT("no")) <<
                   TEXT("\r\n    max bitrate: ")  << IntString(max_bitrate);

        if(!bUseCBR)
        {
            strInfo << TEXT("\r\n    buffer size: ") << IntString(encode_tasks[0].bs.MaxLength*8/1000);
        }

        return strInfo;
    }

    virtual bool DynamicBitrateSupported() const
    {
        return false;
    }

    virtual bool SetBitRate(DWORD maxBitrate, DWORD bufferSize)
    {
        return false;
    }

    virtual void RequestKeyframe()
    {
        bRequestKeyframe = true;
    }

    virtual bool isQSV() { return true; }

    virtual bool HasBufferedFrames()
    {
        return false;
    }
};

VideoEncoder* CreateQSVEncoder(int fps, int width, int height, int quality, CTSTR preset, bool bUse444, ColorDescription &colorDesc, int maxBitRate, int bufferSize, bool bUseCFR, String &errors)
{
    if (!CheckQSVHardwareSupport())
    {
        errors << Str("Encoder.QSV.NoHardwareSupport");
        return nullptr;
    }
    
    if (qsv_get_cpu_platform() <= QSV_CPU_PLATFORM_IVB)
    {
        auto append_help = [&] { errors << Str("Encoder.QSV.ExceededResolutionHelp"); }; //might need to merge this into the actual error messages if there are problems with translations

        if (width > 1920 && height > 1200)
        {
            Log(TEXT("QSV: Output resolution of %ux%u exceeds the maximum of 1920x1200 supported by QuickSync on Sandy Bridge and Ivy Bridge based processors"), width, height);
            errors << FormattedString(Str("Encoder.QSV.SNBIVBMaximumResolutionWidthHeightExceeded"), width, height);
            append_help();
            return nullptr;
        }
        else if (width > 1920)
        {
            Log(TEXT("QSV: Output resolution width of %u exceeds the maximum of 1920 supported by QuickSync on Sandy Bridge and Ivy Bridge based processors"), width);
            errors << FormattedString(Str("Encoder.QSV.SNBIVBMaximumResolutionWidthExceeded"), width);
            append_help();
            return nullptr;
        }
        else if (height > 1200)
        {
            Log(TEXT("QSV: Output resolution height of %u exceeds the maximum of 1200 supported by QuickSync on Sandy Bridge and Ivy Bridge based processors"), height);
            errors << FormattedString(Str("Encoder.QSV.SNBIVBMaximumResolutionHeightExceeded"), height);
            append_help();
            return nullptr;
        }
    }

    return new QSVEncoder(fps, width, height, quality, preset, bUse444, colorDesc, maxBitRate, bufferSize, bUseCFR);
}
