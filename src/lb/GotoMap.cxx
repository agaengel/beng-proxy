/*
 * Copyright 2007-2017 Content Management AG
 * All rights reserved.
 *
 * author: Max Kellermann <mk@cm4all.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the
 * distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "GotoMap.hxx"
#include "Goto.hxx"
#include "Config.hxx"
#include "LuaInitHook.hxx"
#include "avahi/Client.hxx"

void
LbGotoMap::FlushCaches()
{
    for (auto &i : translation_handlers)
        i.second.FlushCache();
}

void
LbGotoMap::InvalidateTranslationCaches(const TranslationInvalidateRequest &request)
{
    for (auto &i : translation_handlers)
        i.second.InvalidateCache(request);
}

LbGoto
LbGotoMap::GetInstance(const char *name)
{
    return GetInstance(root_config.FindGoto(name));
}

LbGoto
LbGotoMap::GetInstance(const LbGotoConfig &config)
{
    if (config.cluster != nullptr)
        return GetInstance(*config.cluster);
    else if (config.branch != nullptr)
        return GetInstance(*config.branch);
    else if (config.lua != nullptr)
        return GetInstance(*config.lua);
    else if (config.translation != nullptr)
        return GetInstance(*config.translation);
    else if (config.response.IsDefined())
        return config.response;
    else
        return LbGoto();
}

LbCluster &
LbGotoMap::GetInstance(const LbClusterConfig &config)
{
    return clusters.emplace(std::piecewise_construct,
                            std::forward_as_tuple(&config),
                            std::forward_as_tuple(config, avahi_client))
        .first->second;
}

LbBranch &
LbGotoMap::GetInstance(const LbBranchConfig &config)
{
    return branches.emplace(std::piecewise_construct,
                            std::forward_as_tuple(&config),
                            std::forward_as_tuple(*this, config))
        .first->second;
}

LbLuaHandler &
LbGotoMap::GetInstance(const LbLuaHandlerConfig &config)
{
    return lua_handlers.emplace(std::piecewise_construct,
                                std::forward_as_tuple(&config),
                                std::forward_as_tuple(lua_init_hook, config))
        .first->second;
}

LbTranslationHandler &
LbGotoMap::GetInstance(const LbTranslationHandlerConfig &config)
{
    return translation_handlers.emplace(std::piecewise_construct,
                                        std::forward_as_tuple(&config),
                                        std::forward_as_tuple(avahi_client.GetEventLoop(),
                                                              *this, config))
        .first->second;
}
