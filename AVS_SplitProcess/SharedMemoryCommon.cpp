#define UNICODE
#define _UNICODE

#include "stdafx.h"
#include "SharedMemoryCommon.h"

#include <stdio.h>
#include <stdexcept>
#include <assert.h>
#include <sstream>
#include "utils.h"

using namespace std;

ClipSyncGroup::ClipSyncGroup(const sys_string& name, int clip_index, shared_memory_clip_info_t& clip_info)
{
    for (int i = 0; i < 2; i++)
    {
        tostringstream ss;
        ss << name << SYSTEXT("_") << clip_index << SYSTEXT("_Response") << i;
        response_conds.push_back(unique_ptr<CondVar>(new CondVar(&clip_info.frame_response[i].lock, ss.str(), FALSE)));
    }
}

sys_string get_shared_memory_key(const char* key1, int key2)
{
    if (strlen(key1) > 256)
    {
        throw invalid_argument("key1 is too long.");
    }
    SYSCHAR buffer[1024];
    memset(buffer, 0, sizeof(buffer));
    _sntprintf(buffer, ARRAYSIZE(buffer) - 1, L"SharedMemorySource_%hs_%d", key1, key2);
    return sys_string(buffer);
}

int get_response_index(int frame_number)
{
    return frame_number & 1;
}


void add_guard_bytes(void* address, DWORD buffer_size)
{
    for (int i = 0; i < 4; i++)
    {
        *(((int*)address) - i - 1) = 0xDEADC0DE;
    }
    for (int i = 0; i < 4; i++)
    {
        *(((int*)address + buffer_size) + i) = 0xDEADBEEF;
    }
}

void check_guard_bytes(void* address, DWORD buffer_size)
{
#if _DEBUG
    for (int i = 0; i < 4; i++)
    {
        assert(*(((int*)address) - i - 1) == 0xDEADC0DE);
    }
    for (int i = 0; i < 4; i++)
    {
        assert(*(((int*)address + buffer_size) + i) == 0xDEADBEEF);
    }
#endif
}

void SharedMemorySourceManager::init_server(const SYSCHAR* mapping_name, int clip_count, const VideoInfo vi_array[])
{
    assert(clip_count > 0);

    // we will copy the structures to the right place later, 
    // so we don't need to align the allocation here
    unique_ptr<shared_memory_clip_info_t[]> info_array(new shared_memory_clip_info_t[clip_count]);
    size_t clip_info_size = sizeof(shared_memory_clip_info_t) * clip_count;
    memset(&info_array[0], 0, clip_info_size);

    DWORD mapping_size = aligned(sizeof(shared_memory_source_header_t) + clip_info_size);
    for (int i = 0; i < clip_count; i++)
    {
        shared_memory_clip_info_t& info = info_array[i];
        info.vi = vi_array[i];
        DWORD clip_buffer_size = aligned(info.vi.RowSize()) * info.vi.height;
        info.frame_pitch = aligned(info.vi.RowSize(), FRAME_ALIGN);

        if (info.vi.IsPlanar() && !info.vi.IsY8())
        {
            DWORD y_size = clip_buffer_size;
            int uv_height = info.vi.height >> info.vi.GetPlaneHeightSubsampling(PLANAR_U);
            int uv_row_size = info.vi.RowSize(PLANAR_U);
            clip_buffer_size += aligned(uv_row_size) * uv_height * 2;

            info.frame_pitch_uv = aligned(uv_row_size, FRAME_ALIGN);
            info.frame_offset_u = y_size;
            info.frame_offset_v = y_size + info.frame_pitch_uv * uv_height;
        }
        info.frame_buffer_size = clip_buffer_size;

        // add some extra space before and after the buffer for guard bytes
        info.frame_buffer_offset[0] = mapping_size + CACHE_LINE_SIZE * 2;
        mapping_size = aligned(mapping_size + clip_buffer_size + 2048);

        info.frame_buffer_offset[1] = mapping_size + CACHE_LINE_SIZE * 2;
        mapping_size = aligned(mapping_size + clip_buffer_size + 2048);
    }

    _mapping_handle.replace(CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, mapping_size, mapping_name));
    if (!_mapping_handle.is_valid())
    {
        DWORD error_code = GetLastError();
        assert(false);
        throw runtime_error("Unable to create file mapping object.");
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        throw runtime_error("The file mapping object already exists.");
    }
    map_view();

    memset(&header, 0, sizeof(shared_memory_source_header_t));
    header->signature = SHARED_MEMORY_SOURCE_SIGNATURE;
    header->clip_count = clip_count;
    memcpy(header->clips, &info_array[0], clip_info_size);
    for (int i = 0; i < clip_count; i++)
    {
        add_guard_bytes(header + header->clips[i].frame_buffer_offset[0], header->clips[i].frame_buffer_size);
        add_guard_bytes(header + header->clips[i].frame_buffer_offset[1], header->clips[i].frame_buffer_size);
    }

}

void SharedMemorySourceManager::map_view()
{
    header = (shared_memory_source_header_t*)MapViewOfFile(_mapping_handle.get(), FILE_MAP_WRITE, 0, 0, 0);
    if (!header)
    {
        DWORD error_code = GetLastError();
        assert(false);
        throw runtime_error("MapViewOfFile failed.");
    }
    if (header->signature != SHARED_MEMORY_SOURCE_SIGNATURE)
    {
        assert(false);
        throw runtime_error("Invalid shared memory object.");
    }
}

void SharedMemorySourceManager::check_data_buffer_integrity(int clip_index, int response_object_id)
{
    assert(clip_index >= 0 && clip_index < header->clip_count);
    assert(response_object_id >= 0 && response_object_id <= 1);
    check_guard_bytes(
        header + header->clips[clip_index].frame_buffer_offset[response_object_id], 
        header->clips[clip_index].frame_buffer_size);
}

void SharedMemorySourceManager::signal_shutdown()
{
    if (_is_server)
    {
        header->shutdown = true;
        request_cond->signal.set();
        for (size_t i = 0; i < sync_groups.size(); i++)
        {
            auto& conds = sync_groups[i]->response_conds;
            for (size_t j = 0; j < conds.size(); j++)
            {
                conds[j]->signal.set();
            }
        }
    }
}

void SharedMemorySourceManager::init_client(const SYSCHAR* mapping_name)
{
    _mapping_handle.replace(OpenFileMapping(FILE_MAP_WRITE, FALSE, mapping_name));
    if (!_mapping_handle.is_valid())
    {
        DWORD error_code = GetLastError();
        assert(false);
        throw runtime_error("Unable to open the file mapping object, maybe the server is closed.");
    }
    map_view();
    InterlockedIncrement(&header->client_count);
}

void SharedMemorySourceManager::init_sync_objects(const sys_string& key, int clip_count)
{
    tstring cond_event_name(key);
    cond_event_name.append(SYSTEXT("_CondEvent"));
    request_cond = unique_ptr<CondVar>(new CondVar(&header->request_lock, cond_event_name, FALSE));
    for (int i = 0; i < clip_count; i++)
    {
        sync_groups.push_back(unique_ptr<ClipSyncGroup>(new ClipSyncGroup(key, i, header->clips[i])));
    }
}

sys_string get_mapping_name(const sys_string& key)
{
    sys_string mapping_name(key);
    mapping_name.append(SYSTEXT("_SharedMemoryObject"));
    return mapping_name;
}

SharedMemorySourceManager::SharedMemorySourceManager(const sys_string key, int clip_count, const VideoInfo vi_array[]) :
    _is_server(true),
    _mapping_handle(NULL),
    header(NULL)
{
    if (clip_count <= 0)
    {
        throw invalid_argument("clip_count must be greater than 0.");
    }


    init_server(get_mapping_name(key).c_str(), clip_count, vi_array);
    init_sync_objects(key, clip_count);
}
SharedMemorySourceManager::SharedMemorySourceManager(const sys_string key) :
    _is_server(false),
    _mapping_handle(NULL),
    header(NULL)
{
    init_client(get_mapping_name(key).c_str());
    init_sync_objects(key, header->clip_count);
}

SharedMemorySourceManager::~SharedMemorySourceManager()
{
    if (!_is_server)
    {
        InterlockedDecrement(&header->client_count);
    } else {
        while (header->client_count > 0)
        {
            signal_shutdown();
            Sleep(1);
        }
    }
    UnmapViewOfFile(header);
    header = NULL;
}
