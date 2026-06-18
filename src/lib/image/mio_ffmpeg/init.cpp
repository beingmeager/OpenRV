//
// Copyright (C) 2023  Autodesk, Inc. All Rights Reserved.
//
// SPDX-License-Identifier: Apache-2.0
//
#include <MovieFFMpeg/MovieFFMpeg.h>

#include <TwkFB/IO.h>
#include <iostream>
#include <fstream>
#include <set>
#include <vector>
#include <string>
#include <boost/program_options.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/split.hpp>

#if defined(PLATFORM_WINDOWS)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

using namespace TwkFB;
using namespace std;
using namespace boost::program_options;
using namespace boost::algorithm;
using namespace boost;

// NOTE ON PATENT LICENSING:
//
//   Legal Note: Some of the codecs listed below contain proprietary
//   algorithms that are protected under intellectual property
//   rights. Please check with your legal department whether you have
//   the proper licenses and rights to use these codecs. TWEAK is not
//   responsible for any unlicensed use of these codecs.
//
//   Upshot: You should not remove entries from this list if you don't
//   actually have that license.
//

static const char* disallowedCodecsArray[] = {
#if !defined(__FFMPEG_ENABLE_NON_FREE_DECODER_ac3)
    "ac3",
#endif
#if !defined(__FFMPEG_ENABLE_NON_FREE_DECODER_hevc)
    "hevc",
#endif
#if !defined(__FFMPEG_ENABLE_NON_FREE_DECODER_mpeg2video)
    "mpeg2video",
#endif
#if !defined(__FFMPEG_ENABLE_NON_FREE_DECODER_prores)
    "prores",
#endif
#if !defined(__FFMPEG_ENABLE_NON_FREE_DECODER_prores_aw)
    "prores_aw",
#endif
#if !defined(__FFMPEG_ENABLE_NON_FREE_DECODER_prores_ks)
    "prores_ks",
#endif
#if !defined(__FFMPEG_ENABLE_NON_FREE_DECODER_prores_lgpl)
    "prores_lgpl",
#endif
#if !defined(__FFMPEG_ENABLE_NON_FREE_DECODER_svq1)
    "svq1",
#endif
#if !defined(__FFMPEG_ENABLE_NON_FREE_DECODER_svq3)
    "svq3",
#endif
    0};

// Runtime non-free codec allowlist.
//
//   The disallowedCodecsArray above is fixed at compile time. On a machine that
//   is licensed to use one or more of those codecs (and has replaced the FFmpeg
//   DLLs with a full-codec build), the user can re-enable them WITHOUT
//   recompiling RV, by either:
//     - setting RV_NONFREE_CODECS (e.g. "prores;dnxhd" or "all"), or
//     - dropping a "nonfree-codecs.txt" file next to this library
//       (on an install that is {app}\PlugIns\MovieFormats\, the directory
//       holding mio_ffmpeg.dll -- NOT {app}\bin where the FFmpeg DLLs live).
//
//   Absent/empty allowlist => identical to the original behavior (disallowed
//   codecs stay disallowed). "all" (or "*") unlocks every name the DLL provides,
//   including those blocked even in the private build (e.g. hevc, mpeg2video).
//
namespace
{
    struct NonFreeAllowlist
    {
        bool all = false;
        std::set<std::string> names;
    };

    // Directory containing this shared library (mio_ffmpeg.dll), where
    // nonfree-codecs.txt is read from -- on an install that is
    // {app}\PlugIns\MovieFormats\, not {app}\bin.
    std::string moduleDir()
    {
#if defined(PLATFORM_WINDOWS)
        HMODULE hm = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(&moduleDir), &hm)
            && hm)
        {
            char path[MAX_PATH] = {0};
            DWORD n = GetModuleFileNameA(hm, path, MAX_PATH);
            if (n > 0 && n < MAX_PATH)
            {
                std::string p(path, n);
                size_t slash = p.find_last_of("\\/");
                if (slash != std::string::npos)
                    return p.substr(0, slash);
            }
        }
#else
        Dl_info info;
        if (dladdr(reinterpret_cast<void*>(&moduleDir), &info) && info.dli_fname)
        {
            std::string p(info.dli_fname);
            size_t slash = p.find_last_of('/');
            if (slash != std::string::npos)
                return p.substr(0, slash);
        }
#endif
        return std::string();
    }

    // Split on separators common to env vars and text files; '#' starts a
    // comment token. Names are lowercased to match FFmpeg's codec names.
    void addTokens(const std::string& text, NonFreeAllowlist& out)
    {
        std::vector<std::string> tokens;
        boost::split(tokens, text, boost::is_any_of(";, \t\r\n"), boost::token_compress_on);
        for (std::string tok : tokens)
        {
            boost::trim(tok);
            if (tok.empty() || tok[0] == '#')
                continue;
            boost::to_lower(tok);
            if (tok == "all" || tok == "*")
                out.all = true;
            else
                out.names.insert(tok);
        }
    }

    const NonFreeAllowlist& nonFreeAllowlist()
    {
        static const NonFreeAllowlist allowlist = []()
        {
            NonFreeAllowlist a;

            if (const char* env = getenv("RV_NONFREE_CODECS"))
                addTokens(env, a);

            std::string dir = moduleDir();
            if (!dir.empty())
            {
                std::ifstream in((dir + "/nonfree-codecs.txt").c_str());
                if (in)
                {
                    std::string line;
                    while (std::getline(in, line))
                        addTokens(line, a);
                }
            }

            return a;
        }();
        return allowlist;
    }
} // namespace

extern "C"
{

#ifdef PLATFORM_WINDOWS
    __declspec(dllexport) TwkMovie::MovieIO* create();
    __declspec(dllexport) void destroy(TwkMovie::MovieFFMpegIO*);
    __declspec(dllexport) bool codecIsAllowed(std::string, bool);
#endif

    static bool codecIsAllowed(std::string name, bool forRead = true)
    {
        bool disallowed = false;
        for (const char** p = disallowedCodecsArray; *p; p++)
        {
            if (*p == name)
            {
                disallowed = true;
                break;
            }
        }

        if (!disallowed)
            return true;

        // Compiled-out codec: a runtime allowlist can re-enable it on a
        // licensed machine without rebuilding RV.
        const NonFreeAllowlist& allow = nonFreeAllowlist();
        if (allow.all || allow.names.count(boost::to_lower_copy(name)))
            return true;

        return false;
    };

    TwkMovie::MovieIO* create()
    {
        int bruteForce = 0;
        int codecThreads = 0;
        double defaultFPS = 24.0;
        string language = "eng";
        if (const char* args = getenv("MOVIEFFMPEG_ARGS"))
        {
            try
            {
                vector<string> buffer;
                split(buffer, args, is_any_of(" "), token_compress_on);

                vector<const char*> newargs(buffer.size() + 1);

                newargs[0] = "";
                for (size_t i = 0; i < buffer.size(); i++)
                    newargs[i + 1] = buffer[i].c_str();

                const char** argv = &newargs.front();
                int argc = newargs.size();

                options_description desc("");
                desc.add_options()("bruteForce", value<int>(&bruteForce)->default_value(bruteForce), "Attempt to load any file extension")(
                    "codecThreads", value<int>(&codecThreads)->default_value(codecThreads), "Thread count to pass to ffmpeg on codec open")(
                    "language", value<string>(&language)->default_value(language),
                    "Language for audio and subtitles (default: eng)")("defaultFPS", value<double>(&defaultFPS)->default_value(defaultFPS),
                                                                       "For cases where there is no way to determine FPS");

                variables_map vm;
                store(parse_command_line(argc, argv, desc), vm);
                notify(vm);
            }
            catch (std::exception& e)
            {
                cout << "ERROR: MOVIEFFMPEG_ARGS: " << e.what() << endl;
                cout << "ERROR: MOVIEFFMPEG_ARGS = \"" << args << "\"" << endl;
            }
            catch (...)
            {
                cout << "ERROR: bad MOVIEFFMPEG_ARGS = \"" << args << "\"" << endl;
            }
        }

        return new TwkMovie::MovieFFMpegIO(codecIsAllowed, bruteForce, codecThreads, language, defaultFPS);
    }

    void destroy(TwkMovie::MovieFFMpegIO* plug) { delete plug; }

} // extern  "C"
