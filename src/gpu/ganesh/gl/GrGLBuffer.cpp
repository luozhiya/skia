/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/gpu/ganesh/gl/GrGLBuffer.h"

#include "include/core/SkTraceMemoryDump.h"
#include "src/core/SkTraceEvent.h"
#include "src/gpu/ganesh/GrGpuResourcePriv.h"
#include "src/gpu/ganesh/gl/GrGLCaps.h"
#include "src/gpu/ganesh/gl/GrGLGpu.h"

#define GL_CALL(X) GR_GL_CALL(this->glGpu()->glInterface(), X)
#define GL_CALL_RET(RET, X) GR_GL_CALL_RET(this->glGpu()->glInterface(), RET, X)

#define GL_ALLOC_CALL(gpu, call)                             \
    [&] {                                                    \
        if (gpu->glCaps().skipErrorChecks()) {               \
            GR_GL_CALL(gpu->glInterface(), call);            \
            return static_cast<GrGLenum>(GR_GL_NO_ERROR);    \
        } else {                                             \
            gpu->clearErrorsAndCheckForOOM();                \
            GR_GL_CALL_NOERRCHECK(gpu->glInterface(), call); \
            return gpu->getErrorAndCheckForOOM();            \
        }                                                    \
    }()

sk_sp<GrGLBuffer> GrGLBuffer::Make(GrGLGpu* gpu,
                                   size_t size,
                                   GrGpuBufferType intendedType,
                                   GrAccessPattern accessPattern) {
    if (gpu->glCaps().transferBufferType() == GrGLCaps::TransferBufferType::kNone &&
        (GrGpuBufferType::kXferCpuToGpu == intendedType ||
         GrGpuBufferType::kXferGpuToCpu == intendedType)) {
        return nullptr;
    }

    sk_sp<GrGLBuffer> buffer(new GrGLBuffer(gpu, size, intendedType, accessPattern,
                                            /*label=*/"MakeGlBuffer"));
    if (0 == buffer->bufferID()) {
        return nullptr;
    }
    return buffer;
}

// GL_STREAM_DRAW triggers an optimization in Chromium's GPU process where a client's vertex buffer
// objects are implemented as client-side-arrays on tile-deferred architectures.
#define DYNAMIC_DRAW_PARAM GR_GL_STREAM_DRAW

inline static GrGLenum gr_to_gl_access_pattern(GrGpuBufferType bufferType,
                                               GrAccessPattern accessPattern,
                                               const GrGLCaps& caps) {
    auto drawUsage = [](GrAccessPattern pattern) {
        switch (pattern) {
            case kDynamic_GrAccessPattern:
                // TODO: Do we really want to use STREAM_DRAW here on non-Chromium?
                return DYNAMIC_DRAW_PARAM;
            case kStatic_GrAccessPattern:
                return GR_GL_STATIC_DRAW;
            case kStream_GrAccessPattern:
                return GR_GL_STREAM_DRAW;
        }
        SkUNREACHABLE;
    };

    auto readUsage = [](GrAccessPattern pattern) {
        switch (pattern) {
            case kDynamic_GrAccessPattern:
                return GR_GL_DYNAMIC_READ;
            case kStatic_GrAccessPattern:
                return GR_GL_STATIC_READ;
            case kStream_GrAccessPattern:
                return GR_GL_STREAM_READ;
        }
        SkUNREACHABLE;
    };

    auto usageType = [&drawUsage, &readUsage, &caps](GrGpuBufferType type,
                                                     GrAccessPattern pattern) {
        // GL_NV_pixel_buffer_object adds transfer buffers but not the related <usage> values.
        if (caps.transferBufferType() == GrGLCaps::TransferBufferType::kNV_PBO) {
            return drawUsage(pattern);
        }
        switch (type) {
            case GrGpuBufferType::kVertex:
            case GrGpuBufferType::kIndex:
            case GrGpuBufferType::kDrawIndirect:
            case GrGpuBufferType::kXferCpuToGpu:
            case GrGpuBufferType::kUniform:
                return drawUsage(pattern);
            case GrGpuBufferType::kXferGpuToCpu:
                return readUsage(pattern);
        }
        SkUNREACHABLE;
    };

    return usageType(bufferType, accessPattern);
}

GrGLBuffer::GrGLBuffer(GrGLGpu* gpu,
                       size_t size,
                       GrGpuBufferType intendedType,
                       GrAccessPattern accessPattern,
                       std::string_view label)
        : INHERITED(gpu, size, intendedType, accessPattern, label)
        , fIntendedType(intendedType)
        , fBufferID(0)
        , fUsage(gr_to_gl_access_pattern(intendedType, accessPattern, gpu->glCaps()))
        , fHasAttachedToTexture(false) {
    GL_CALL(GenBuffers(1, &fBufferID));
    if (fBufferID) {
        GrGLenum target = gpu->bindBuffer(fIntendedType, this);
        GrGLenum error = GL_ALLOC_CALL(this->glGpu(), BufferData(target,
                                                                 (GrGLsizeiptr)size,
                                                                 nullptr,
                                                                 fUsage));
        if (error != GR_GL_NO_ERROR) {
            GL_CALL(DeleteBuffers(1, &fBufferID));
            fBufferID = 0;
        }
    }
    this->registerWithCache(SkBudgeted::kYes);
    if (!fBufferID) {
        this->resourcePriv().removeScratchKey();
    }
}

inline GrGLGpu* GrGLBuffer::glGpu() const {
    SkASSERT(!this->wasDestroyed());
    return static_cast<GrGLGpu*>(this->getGpu());
}

inline const GrGLCaps& GrGLBuffer::glCaps() const {
    return this->glGpu()->glCaps();
}

void GrGLBuffer::onRelease() {
    TRACE_EVENT0("skia.gpu", TRACE_FUNC);

    if (!this->wasDestroyed()) {
        // make sure we've not been abandoned or already released
        if (fBufferID) {
            GL_CALL(DeleteBuffers(1, &fBufferID));
            fBufferID = 0;
        }
        fMapPtr = nullptr;
    }

    INHERITED::onRelease();
}

void GrGLBuffer::onAbandon() {
    fBufferID = 0;
    fMapPtr = nullptr;
    INHERITED::onAbandon();
}

static inline GrGLenum SK_WARN_UNUSED_RESULT invalidate_buffer(GrGLGpu* gpu,
                                                               GrGLenum target,
                                                               GrGLenum usage,
                                                               GrGLuint bufferID,
                                                               size_t bufferSize) {
    switch (gpu->glCaps().invalidateBufferType()) {
        case GrGLCaps::InvalidateBufferType::kNone:
            return GR_GL_NO_ERROR;
        case GrGLCaps::InvalidateBufferType::kNullData:
            return GL_ALLOC_CALL(gpu, BufferData(target, bufferSize, nullptr, usage));
        case GrGLCaps::InvalidateBufferType::kInvalidate:
            GR_GL_CALL(gpu->glInterface(), InvalidateBufferData(bufferID));
            return GR_GL_NO_ERROR;
    }
    SkUNREACHABLE;
}

void GrGLBuffer::onMap() {
    SkASSERT(fBufferID);
    SkASSERT(!this->wasDestroyed());
    SkASSERT(!this->isMapped());

    // TODO: Make this a function parameter.
    bool readOnly = (GrGpuBufferType::kXferGpuToCpu == fIntendedType);

    // Handling dirty context is done in the bindBuffer call
    switch (this->glCaps().mapBufferType()) {
        case GrGLCaps::kNone_MapBufferType:
            return;
        case GrGLCaps::kMapBuffer_MapBufferType: {
            GrGLenum target = this->glGpu()->bindBuffer(fIntendedType, this);
            if (!readOnly) {
                GrGLenum error = invalidate_buffer(this->glGpu(),
                                                   target,
                                                   fUsage,
                                                   fBufferID,
                                                   this->size());
                if (error != GR_GL_NO_ERROR) {
                    return;
                }
            }
            GL_CALL_RET(fMapPtr, MapBuffer(target, readOnly ? GR_GL_READ_ONLY : GR_GL_WRITE_ONLY));
            break;
        }
        case GrGLCaps::kMapBufferRange_MapBufferType: {
            GrGLenum target = this->glGpu()->bindBuffer(fIntendedType, this);
            GrGLbitfield access;
            if (readOnly) {
                access = GR_GL_MAP_READ_BIT;
            } else {
                access = GR_GL_MAP_WRITE_BIT;
                if (GrGpuBufferType::kXferCpuToGpu != fIntendedType) {
                    // TODO: Make this a function parameter.
                    access |= GR_GL_MAP_INVALIDATE_BUFFER_BIT;
                }
            }
            GL_CALL_RET(fMapPtr, MapBufferRange(target, 0, this->size(), access));
            break;
        }
        case GrGLCaps::kChromium_MapBufferType: {
            GrGLenum target = this->glGpu()->bindBuffer(fIntendedType, this);
            GL_CALL_RET(fMapPtr, MapBufferSubData(target, 0, this->size(),
                                                  readOnly ? GR_GL_READ_ONLY : GR_GL_WRITE_ONLY));
            break;
        }
    }
}

void GrGLBuffer::onUnmap() {
    SkASSERT(fBufferID);
    SkASSERT(this->isMapped());
    if (0 == fBufferID) {
        fMapPtr = nullptr;
        return;
    }
    // bind buffer handles the dirty context
    switch (this->glCaps().mapBufferType()) {
        case GrGLCaps::kNone_MapBufferType:
            SkDEBUGFAIL("Shouldn't get here.");
            return;
        case GrGLCaps::kMapBuffer_MapBufferType: // fall through
        case GrGLCaps::kMapBufferRange_MapBufferType: {
            GrGLenum target = this->glGpu()->bindBuffer(fIntendedType, this);
            GL_CALL(UnmapBuffer(target));
            break;
        }
        case GrGLCaps::kChromium_MapBufferType:
            this->glGpu()->bindBuffer(fIntendedType, this); // TODO: Is this needed?
            GL_CALL(UnmapBufferSubData(fMapPtr));
            break;
    }
    fMapPtr = nullptr;
}

bool GrGLBuffer::onUpdateData(const void* src, size_t srcSizeInBytes) {
    SkASSERT(fBufferID);

    // bindbuffer handles dirty context
    GrGLenum target = this->glGpu()->bindBuffer(fIntendedType, this);
    GrGLenum error = invalidate_buffer(this->glGpu(), target, fUsage, fBufferID, this->size());
    if (error != GR_GL_NO_ERROR) {
        return false;
    }
    GL_CALL(BufferSubData(target, 0, (GrGLsizeiptr)srcSizeInBytes, src));
    return true;
}

void GrGLBuffer::onSetLabel() {
    SkASSERT(fBufferID);
    if (!this->getLabel().empty()) {
        const std::string label = "_Skia_" + this->getLabel();
        if (this->glGpu()->glCaps().debugSupport()) {
            GL_CALL(ObjectLabel(GR_GL_BUFFER, fBufferID, -1, label.c_str()));
        }
    }
}

void GrGLBuffer::setMemoryBacking(SkTraceMemoryDump* traceMemoryDump,
                                  const SkString& dumpName) const {
    SkString buffer_id;
    buffer_id.appendU32(this->bufferID());
    traceMemoryDump->setMemoryBacking(dumpName.c_str(), "gl_buffer", buffer_id.c_str());
}
