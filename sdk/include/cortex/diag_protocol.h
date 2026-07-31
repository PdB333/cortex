#pragma once

#include <windows.h>
#include <stdint.h>

#define CORTEX_DIAG_SHARED_MAGIC 0x58445243u /* CRDX */
#define CORTEX_DIAG_SHARED_VERSION 1u
#define CORTEX_DIAG_MAX_HEARTBEATS 32u
#define CORTEX_DIAG_HEARTBEAT_NAME_SIZE 32u
#define CORTEX_DIAG_MAPPING_PREFIX "Local\\CortexDiag_"
#define CORTEX_DIAG_EVENT_PREFIX "Local\\CortexDiagEvent_"

typedef enum CortexDiagSharedAccessType {
    CORTEX_DIAG_ACCESS_UNKNOWN = 0,
    CORTEX_DIAG_ACCESS_READ = 1,
    CORTEX_DIAG_ACCESS_WRITE = 2,
    CORTEX_DIAG_ACCESS_EXECUTE = 3
} CortexDiagSharedAccessType;

typedef struct CortexDiagSharedHeartbeat {
    char source[CORTEX_DIAG_HEARTBEAT_NAME_SIZE];
    DWORD thread_id;
    DWORD reserved;
    __declspec(align(8)) volatile LONG64 last_tick_ms;
    __declspec(align(8)) volatile LONG64 sequence;
} CortexDiagSharedHeartbeat;

typedef struct CortexDiagSharedCrash {
    volatile LONG sequence;
    DWORD thread_id;
    DWORD exception_code;
    DWORD access_type;
    uint64_t exception_address;
    uint64_t accessed_address;
    __declspec(align(16)) CONTEXT context;
} CortexDiagSharedCrash;

typedef struct CortexDiagSharedState {
    uint32_t magic;
    uint32_t version;
    DWORD process_id;
    uint32_t pointer_size;
    volatile LONG ready;
    volatile LONG lock;
    __declspec(align(8)) volatile LONG64 started_tick_ms;
    __declspec(align(8)) volatile LONG64 last_core_heartbeat_ms;
    volatile LONG heartbeat_count;
    DWORD reserved;
    CortexDiagSharedHeartbeat heartbeats[CORTEX_DIAG_MAX_HEARTBEATS];
    CortexDiagSharedCrash crash;
} CortexDiagSharedState;
