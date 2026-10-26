foreach(_pipeline RenderPipeline ComputePipeline)
    set(_source "${DAWN_SOURCE_DIR}/src/dawn/native/d3d12/${_pipeline}D3D12.cpp")
    if (NOT EXISTS "${_source}")
        message(FATAL_ERROR "Dawn pipeline source not found: ${_source}")
    endif ()
    file(READ "${_source}" _code)
    if (_code MATCHES "#ifndef MELEE_XBOX_UWP")
        continue()
    endif ()
    set(_load "Blob blob = device->LoadCachedBlob(GetCacheKey());")
    set(_store "device->StoreCachedBlob(GetCacheKey(), CreateBlob(std::move(d3dBlob)));")
    string(FIND "${_code}" "${_load}" _load_pos)
    string(FIND "${_code}" "${_store}" _store_pos)
    if (_load_pos EQUAL -1 OR _store_pos EQUAL -1)
        message(FATAL_ERROR "Dawn cache code changed: ${_source}")
    endif ()
    string(REPLACE "${_load}"
        "Blob blob;\n#ifndef MELEE_XBOX_UWP\n    blob = device->LoadCachedBlob(GetCacheKey());\n#endif"
        _code "${_code}")
    string(REPLACE "ComPtr<ID3DBlob> d3dBlob;"
        "#ifndef MELEE_XBOX_UWP\n        ComPtr<ID3DBlob> d3dBlob;"
        _code "${_code}")
    string(REPLACE "${_store}\n        }"
        "${_store}\n        }\n#endif" _code "${_code}")
    file(WRITE "${_source}" "${_code}")
endforeach()
