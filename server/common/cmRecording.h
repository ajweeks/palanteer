// Palanteer recording library
// Copyright (C) 2021, Damien Feneyrou <dfeneyrou@gmail.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <atomic>
#include <thread>

#include "bs.h"
#include "bsVec.h"
#include "bsHashMap.h"
#include "bsString.h"
#include "cmRecord.h"

// Forward declaration
class cmInterface;

class cmRecording {
public:
    cmRecording(cmInterface* itf, const bsString& storagePath, bool doForwardEvents);
    ~cmRecording(void);

    // Core methods
    cmRecord* beginRecord(const bsString& appName, const bsString& buildName, int protocol, s64 timeNsOrigin, double tickToNs,
                          bool areStringsExternal, int cacheMBytes, bsString& errorMsg, bool doCreateLiveRecord);
    void endRecord(void);
    bool isRecording(void) { return _recFd!=0; }
    const bsString& storeNewString(const bsString& newString, u64 hash);
    void storeNewEvents(plPriv::EventExt* events, int eventQty);
    void createDeltaRecord(cmRecord::Delta* delta);
    const bsString& getRecordPath(void) const { return _recordPath; }

    // Accessors
    void setRecordingConfig(bool isEnabled, const char* forcedFilename) {
        _isRecordingEnabled   = isEnabled;
        _forcedRecordFilename = forcedFilename;
    }
    const bsString& getRecordsDataPath(void) const { return _storagePath; }
    void doPauseStoring(bool state);
    u64  getThreadNameHash(int threadId) const { return _recThreads[threadId].threadUniqueHash; }
    int  getThreadNameIdx (int threadId) const { return _recThreads[threadId].nameIdx; }
    void getElemInfos(int elemIdx, u64* elemHash, int* elemPrevElemIdx, int* elemThreadId) {
        *elemHash        = _recStrings[_recElems[elemIdx].nameIdx].hash;
        *elemPrevElemIdx = _recElems[elemIdx].prevElemIdx;
        *elemThreadId    = _recElems[elemIdx].threadId;
    }
    const bsString& getString(int idx) const { return _recStrings[idx].value; } // To call only in a thread-safe way

private:
    cmRecording(const cmRecording& other); // To please static analyzers
    cmRecording& operator=(cmRecording other);

    cmInterface* _itf = 0;
    bool         _isRecordingEnabled = true; // Enabled by default (viewer case)
    bool         _doForwardEvents = false;
    bsString     _forcedRecordFilename;      // Empty means automatic naming

    // Reception
    bsString _storagePath;
    bool     _isCompressionEnabled;
    std::atomic<int> _doStopThread;
    std::thread*     _threadCollectFromClient = 0;
    bsString         _injectedFilename;

    // Parsing
    int  _recordProtocol     = 0;
    int  _areStringsExternal = 0;
    bool _recordToggleBytes  = false;
    bsString _recordName;
    static constexpr int _parseHeaderSize = 8;
    int _parseHeaderDataLeft = _parseHeaderSize;
    int _parseStringLeft = 0;
    int _parseEventLeft  = 0;
    bsVec<u8> _parseTempStorage;
    void resetParser(void) {
        _parseHeaderDataLeft = _parseHeaderSize; _parseStringLeft = 0;
        _parseEventLeft = 0; ; _parseTempStorage.clear();
    }

    // Record
    FILE* _recFd = 0;
    struct VMemAlloc {
        int threadId;
        u32 size;
        u32 mIdx;
        int currentScopeIdx;
    };

    struct LockBuild {
        u32        nameIdx;
        bool       isInUse            = false;
        int        usingStartThreadId = -1;
        s64        usingStartTimeNs   = 0;
        bsVec<int> waitingThreadIds;
    };

    struct ElemBuild {
        u64 hashPath;
        u64 threadBitmap;
        u32 hashKey;
        u32 prevElemIdx; // (u32)-1 if none
        int threadId;
        int nestingLevel;
        u32 nameIdx;
        u32 hlNameIdx;
        int flags;
        bool doRepresentScope; // If true, MR scheme will merge toward density. Else it will merge toward subsampling (for plot typically)
        int  isPartOfHStruct; // If true, this elem is part of the main hierarchical structure, so suitable for searching
        int  isThreadHashed;  // If true, the hashPath has a final step with threadPath
        double absYMin =  1e300;
        double absYMax = -1e300;
        s64    lastTimeNs = 0;
        bool   hasDeltaChanges = false;
        bsVec<u32>    chunkLIdx; // Data here is the LIdx of the corresponding couple (thread/nesting level)
        bsVec<s64>    chunkTimes;
        bsVec<double> chunkValues;
        int                  lastLocIdx = 0;
        bsVec<chunkLoc_t>    chunkLocs;
        bsVec<bsVec<cmRecord::ElemMR>> mrSpeckChunks;  // Meant to be fully in memory
        bsVec<int>           lastMrSpeckChunksIndexes;
        bsVec<bsVec<double>> workMrValues; // Not stored, used to build the pyramid with min/max function on value
    };

    // No storage automata
    struct PauseState {
        plPriv::EventExt unstoredBeginEvt;
        bool isUnstoredScopeOpen = false;
        bool isScopeOpen         = false;
    };

#define LOC_STORAGE_REC(name)                         \
    int                  name##LastLocIdx = 0;        \
    bsVec<cmRecord::Evt> name##ChunkData;             \
    bsVec<chunkLoc_t>    name##ChunkLocs

#define LOC_STORAGE_RESET(name)                         \
    name##LastLocIdx = 0;                               \
    name##ChunkData.clear();                            \
    name##ChunkLocs.clear()

    struct NestingLevelBuild {
        // Level Indexes (lIdx)
        LOC_STORAGE_REC(nonScope);
        LOC_STORAGE_REC(scope);
        // Multi-resolution data
        bsVec<int>        lastMrScopeSpeckChunksIndexes;
        bsVec<bsVec<u32>> mrScopeSpeckChunks; // Meant to be fully in memory
        // Working info
        u64  hashPath   = 0;
        s64  writeScopeLastTimeNs = 0;
        u32  scopeCurrentLIdx = PL_INVALID;
        bool lastIsScope = false; // For generic events. Initial value does not matter
        s64  elemTimeNs     = 0;
        u32  elemLIdx       = 0;
        u32  parentNameIdx  = PL_INVALID;
        u8   parentFlags    = 0;
        u32  prevElemIdx    = (u32)-1;
        PauseState pause;
        // Working memory infos
        u64 beginSumAllocQty    = 0;
        u64 beginSumAllocSize   = 0;
        u64 beginSumDeallocQty  = 0;
        u64 beginSumDeallocSize = 0;
        u64 lastAllocPtr   = 0;
        u64 lastDeallocPtr = 0;
        u32 lastAllocSize  = 0;
    };
    struct ThreadBuild {
        u64 threadHash        = 0;
        u64 threadUniqueHash  = 0;  // Equal to threadHash unless a name is given to the thread
        int nameIdx           = -1;
        int curLevel          = 0;
        u32 elemEventQty      = 0;
        u32 memEventQty       = 0;
        u32 ctxSwitchEventQty = 0;
        u32 lockEventQty      = 0;
        u32 markerEventQty    = 0;
        u32 droppedEventQty   = 0;
        s64 durationNs        = 0;
        // Memory
        u64  sumAllocQty    = 0;
        u64  sumAllocSize   = 0;
        u64  sumDeallocQty  = 0;
        u64  sumDeallocSize = 0;
        bool lastIsAlloc    = false; // Initial value does not matter
        int  memEventQtyBeforeSnapshot = PL_MEMORY_SNAPSHOT_EVENT_INTERVAL;
        bsVec<u32> memSSCurrentAlloc;
        bsVec<int> memSSEmptyIdx;
        bsVec<u32> memDeallocMIdx; // Per alloc mIdx
        int        memDeallocMIdxLastIdx = 0;
        bsVec<cmRecord::MemSnapshot> memSnapshotIndexes;
        int        memSnapshotIndexesLastIdx = 0;
        LOC_STORAGE_REC(memAlloc);
        LOC_STORAGE_REC(memDealloc);
        LOC_STORAGE_REC(memPlot);
        // Context switches & softIrq
        LOC_STORAGE_REC(ctxSwitch);
        LOC_STORAGE_REC(softIrq);
        PauseState softIrqPause;
        // Locks (no need for a pause, as it is also a 'scope')
        LOC_STORAGE_REC(lockWait);
        bsVec<u32> lockWaitNameIdxs;
        bool       lockWaitCurrentlyWaiting = false;
        // Levels
        bsVec<NestingLevelBuild> levels;
    };
    struct GlobalBuild {
        LOC_STORAGE_REC(lockUse);
        LOC_STORAGE_REC(lockNtf);
        LOC_STORAGE_REC(coreUsage);
        LOC_STORAGE_REC(marker);
    };

    void saveThreadMemorySnapshot(ThreadBuild& tc, s64 timeNs, u32 allocMIdx);
    void processScopeEvent     (plPriv::EventExt& evtx, ThreadBuild& tc, int level);
    void processMemoryEvent    (plPriv::EventExt& evtx, ThreadBuild& tc, int level);
    void processCtxSwitchEvent (plPriv::EventExt& evtx, ThreadBuild& tc);
    void processSoftIrqEvent   (plPriv::EventExt& evtx, ThreadBuild& tc);
    bool processCoreUsageEvent (plPriv::EventExt& evtx);
    void processMarkerEvent    (plPriv::EventExt& evtx, ThreadBuild& tc, int level, bool doForwardEvents);
    void processLockNotifyEvent(plPriv::EventExt& evtx, ThreadBuild& tc, int level, bool doForwardEvents);
    void processLockWaitEvent  (plPriv::EventExt& evtx, ThreadBuild& tc, int level);
    bool processLockUseEvent   (plPriv::EventExt& evtx, bool& doInsertLockWaitEnd);
    void writeScopeChunk  (NestingLevelBuild& lc, bool isLast=false);
    void writeElemChunk   (ElemBuild& elem, bool isLast=false);
    void writeGenericChunk(bsVec<cmRecord::Evt>& chunkData, bsVec<chunkLoc_t>& chunkLocs);

    // Structured storage
    s64 _recTimeNsOrigin   = 0;
    double _recTickToNs    = 1.;
    s64 _recDurationNs     = 0;
    u64 _recLastEventFileOffset = 0;
    s64 _recLastCSwitchDateNs = 0;
    int _recCoreQty        = 0;
    int _recUsedCoreCount  = 0;
    u32 _recElemChunkQty   = 0;
    u32 _recElemEventQty   = 0;
    u32 _recMemEventQty    = 0;
    u32 _recLockEventQty   = 0;
    u32 _recMarkerEventQty = 0;
    u32 _recCtxSwitchEventQty = 0;
    int _recLastIdxErrorQty = 0;
    int _recErrorQty        = 0;
    u8  _recCoreIsUsed[256];
    u8  _recCoreIsPaused[256];
    bool _requestPauseStoring  = false;
    bool _requestResumeStoring = false;
    bool _noStoring            = false;
    bsHashMap<u64,VMemAlloc> _recMemAllocLkup;
    bsHashMap<int,int>  _recElemPathToId;
    bsVec<u32>          _recMarkerCategoryNameIdxs;
    bsVec<LockBuild>    _recLocks;
    bsVec<ElemBuild>    _recElems;
    bsVec<PauseState>   _recLockPauses;
    bsVec<ThreadBuild>  _recThreads;
    GlobalBuild         _recGlobal;
    bsVec<cmRecord::String> _recStrings; // We locally use "isHexa" to mark changes. This "hack" avoids a copy of the structure of cmRecord
    bsString            _recordAppName;
    bsString            _recordBuildName;
    bsString            _recordPath;
    cmRecord::RecError  _recErrors[cmRecord::MAX_REC_ERROR_QTY];
    bsHashMap<int,int>  _recErrorLkup;

    // Some working buffer (to avoid creating array and reallocating each time)
    bsVec<u8>               _workingCompressionBuffer; // For compression
    bsVec<u32>              _workingNewMRScopes;     // For scope chunk writing
    bsVec<cmRecord::ElemMR> _workingNewMRElems;      // For Elem chunk writing
    bsVec<double>           _workingNewMRElemValues; // For Elem chunk writing


    // Delta record
    int        _recLastSizeStrings = 0;
    bsVec<int> _recNameUpdatedThreadIds;
    bsVec<u32> _recUpdatedElemIds;
    bsVec<u32> _recUpdatedLockIds;
    bsVec<u32> _recUpdatedStringIds;
};
