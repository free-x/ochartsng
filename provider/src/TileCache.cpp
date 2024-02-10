/******************************************************************************
 *
 * Project:  AvNav ocharts-provider
 * Purpose:  Tile Cache
 * Author:   Andreas Vogel
 *
 ***************************************************************************
 *   Copyright (C) 2024 by Andreas Vogel   *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301,  USA.             *
 ***************************************************************************
 *
 */

#include "TileCache.h"
#include "StringHelper.h"
String TileCache::getKey(const TileInfo &tile){
    return FMT("%s/%d/%d/%d",tile.chartSetKey,tile.zoom,tile.x,tile.y);
}
void TileCache::ToJson(StatusStream &stream){
    stream["numEntries"]=(int)numEntries;
    stream["numKb"]=(int)numKb;
}
class CHelper{
    public:
    String key;
    Timer::SteadyTimePoint lastAccess;
    size_t kb=0;
    CHelper(const String &k, const Timer::SteadyTimePoint &l, size_t s):
        key(k),lastAccess(l),kb(s){}
};
void TileCache::cleanup(){
    if (numKb <= maxMem) return;
    using CHList=std::vector<CHelper>;
    std::unique_ptr<CHList> items=std::make_unique<CHList>();
    {
        Synchronized l(lock);
        items->reserve(cache.size());
        for (auto &&[key,ci]:cache){
            items->push_back(CHelper(key,ci->lastAccess, ci->size));
        }
        std::sort(items->begin(),items->end(),[](CHelper const & c1, CHelper const & c2){
            return c1.lastAccess < c2.lastAccess;
        });
        auto last=items->begin();
        int removeKb=0;
        for (auto it=items->begin();it != items->end();it++){
            removeKb+=it->kb;
            if (removeKb >= (numKb-maxMem)){
                last=it;
                break;
            }
        }
        for(auto it=items->begin();it!= last && it != items->end();it++){
            cache.erase(it->key);
        }
        int newKb=numKb-removeKb;
        if (newKb < 0) newKb=0;
        numKb=newKb;
        numEntries=cache.size();
    }
}
void TileCache::clean(String setKey){
    Synchronized l(lock);
    size_t current=cache.size();
    int currentKb=numKb;
    int newKb=0;
    if (setKey.empty()){
        cache.clear();
        numEntries=0;
        LOG_INFO("deleted %d entries from tile cache, freeing ~ %dkb",current,(int)numKb);
        numKb=0;
        return;
    }
    avnav::erase_if(cache,[setKey,&newKb](Data::reference &item){
        bool rt=StringHelper::startsWith(item.first,setKey);
        if (!rt){
            int kb=item.second->size;
            newKb+=kb;
        }
        return rt;
    });
    numKb=newKb;
    if (cache.size() != current){
        LOG_INFO("clean: deleted %d entries from tile cache, freeing ~ %dkb",current,(currentKb-newKb));
    }
    numEntries=cache.size();
}
void TileCache::cleanBySettings(int remainingSequence){
    Synchronized l(lock);
    size_t current=cache.size();
    int currentKb=numKb;
    int newKb=0;
    avnav::erase_if(cache,[remainingSequence,&newKb](Data::reference &item){
        bool rt=item.second->description.settingsSequence != remainingSequence;
        if (!rt){
            int kb=item.second->size;
            newKb+=kb;
        }
        return rt;
    });
    numKb=newKb;
    if (cache.size() != current){
        LOG_INFO("cleanBySettings: deleted %d entries from tile cache, freeing ~ %dkb",current,(currentKb-newKb));
    }
    numEntries=cache.size();
}
bool TileCache::addTile(TileCache::Png d, const TileCache::CacheDescription &description, const TileInfo &tile){
    Synchronized l(lock);
    String key=getKey(tile);
    auto cur=cache.find(key);
    bool rt=false;
    if (cur == cache.end()){
        CacheEntry::Ptr ne=std::make_shared<CacheEntry>(d,description,key.size());
        cache[key]=ne;
        numKb+=ne->size;
        rt=true;
    }
    else if (description.isNewer(cur->second->description)){
        int oldKb=cur->second->size/1024;
        CacheEntry::Ptr ne=std::make_shared<CacheEntry>(d,description,key.size());
        cache[key]=ne;
        int newKb=ne->size;
        numKb+=(newKb-oldKb);
        rt=true;
    }
    numEntries=cache.size();
    return rt;
}
TileCache::Png TileCache::getTile(const TileCache::CacheDescription &description, const TileInfo &tile){
    Synchronized l(lock);
    String key=getKey(tile);
    auto cur=cache.find(key);
    if (cur == cache.end()){
        return TileCache::Png();
    }
    if (description.equals(cur->second->description)){
        cur->second->lastAccess=Timer::steadyNow();
        return cur->second->data;
    }
    return TileCache::Png();
}

TileCache::TileCache(size_t max):maxMem(max){
    std::thread([this](){
        while (! this->stopAudit){
            Timer::microSleep(1000000L);
            this->cleanup();
        }
    }).detach();
}
