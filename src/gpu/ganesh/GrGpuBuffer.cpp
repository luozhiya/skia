/*
 * Copyright 2019 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/gpu/ganesh/GrCaps.h"
#include "src/gpu/ganesh/GrGpu.h"
#include "src/gpu/ganesh/GrGpuBuffer.h"

GrGpuBuffer::GrGpuBuffer(GrGpu* gpu, size_t sizeInBytes, GrGpuBufferType type,
                         GrAccessPattern pattern,
                         std::string_view label)
        : GrGpuResource(gpu, label)
        , fMapPtr(nullptr)
        , fSizeInBytes(sizeInBytes)
        , fAccessPattern(pattern)
        , fIntendedType(type) {}

void* GrGpuBuffer::map() {
    if (this->wasDestroyed()) {
        return nullptr;
    }
    if (!fMapPtr) {
        this->onMap();
    }
    return fMapPtr;
}

void GrGpuBuffer::unmap() {
    if (this->wasDestroyed()) {
        return;
    }
    SkASSERT(fMapPtr);
    this->onUnmap();
    fMapPtr = nullptr;
}

bool GrGpuBuffer::isMapped() const { return SkToBool(fMapPtr); }

bool GrGpuBuffer::updateData(const void* src, size_t srcSizeInBytes) {
    SkASSERT(!this->isMapped());
    SkASSERT(srcSizeInBytes > 0 && srcSizeInBytes <= fSizeInBytes);
    SkASSERT(src);
    if (this->wasDestroyed()) {
        return false;
    }
    if (this->intendedType() == GrGpuBufferType::kXferGpuToCpu) {
        return false;
    }
    return this->onUpdateData(src, srcSizeInBytes);
}

void GrGpuBuffer::ComputeScratchKeyForDynamicBuffer(size_t size,
                                                    GrGpuBufferType intendedType,
                                                    skgpu::ScratchKey* key) {
    static const skgpu::ScratchKey::ResourceType kType = skgpu::ScratchKey::GenerateResourceType();
    skgpu::ScratchKey::Builder builder(key, kType, 1 + (sizeof(size_t) + 3) / 4);
    builder[0] = SkToU32(intendedType);
    builder[1] = (uint32_t)size;
    if (sizeof(size_t) > 4) {
        builder[2] = (uint32_t)((uint64_t)size >> 32);
    }
}

void GrGpuBuffer::computeScratchKey(skgpu::ScratchKey* key) const {
    if (SkIsPow2(fSizeInBytes) && kDynamic_GrAccessPattern == fAccessPattern) {
        ComputeScratchKeyForDynamicBuffer(fSizeInBytes, fIntendedType, key);
    }
}
