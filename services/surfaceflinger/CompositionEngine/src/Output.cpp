/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <android-base/stringprintf.h>
#include <compositionengine/CompositionEngine.h>
#include <compositionengine/DisplayColorProfile.h>
#include <compositionengine/LayerFE.h>
#include <compositionengine/RenderSurface.h>
#include <compositionengine/impl/Output.h>
#include <compositionengine/impl/OutputLayer.h>
#include <ui/DebugUtils.h>
#include <utils/Trace.h>

namespace android::compositionengine {

Output::~Output() = default;

namespace impl {

Output::Output(const CompositionEngine& compositionEngine)
      : mCompositionEngine(compositionEngine) {}

Output::~Output() = default;

const CompositionEngine& Output::getCompositionEngine() const {
    return mCompositionEngine;
}

bool Output::isValid() const {
    return mDisplayColorProfile && mDisplayColorProfile->isValid() && mRenderSurface &&
            mRenderSurface->isValid();
}

const std::string& Output::getName() const {
    return mName;
}

void Output::setName(const std::string& name) {
    mName = name;
}

void Output::setCompositionEnabled(bool enabled) {
    if (mState.isEnabled == enabled) {
        return;
    }

    mState.isEnabled = enabled;
    dirtyEntireOutput();
}

void Output::setProjection(const ui::Transform& transform, int32_t orientation, const Rect& frame,
                           const Rect& viewport, const Rect& scissor, bool needsFiltering) {
    mState.transform = transform;
    mState.orientation = orientation;
    mState.scissor = scissor;
    mState.frame = frame;
    mState.viewport = viewport;
    mState.needsFiltering = needsFiltering;

    dirtyEntireOutput();
}

// TODO(lpique): Rename setSize() once more is moved.
void Output::setBounds(const ui::Size& size) {
    mRenderSurface->setDisplaySize(size);
    // TODO(lpique): Rename mState.size once more is moved.
    mState.bounds = Rect(mRenderSurface->getSize());

    dirtyEntireOutput();
}

void Output::setLayerStackFilter(uint32_t layerStackId, bool isInternal) {
    mState.layerStackId = layerStackId;
    mState.layerStackInternal = isInternal;

    dirtyEntireOutput();
}

void Output::setColorTransform(const mat4& transform) {
    if (mState.colorTransformMat == transform) {
        return;
    }

    const bool isIdentity = (transform == mat4());
    const auto newColorTransform =
            isIdentity ? HAL_COLOR_TRANSFORM_IDENTITY : HAL_COLOR_TRANSFORM_ARBITRARY_MATRIX;

    mState.colorTransform = newColorTransform;
    mState.colorTransformMat = transform;

    dirtyEntireOutput();
}

void Output::setColorMode(ui::ColorMode mode, ui::Dataspace dataspace,
                          ui::RenderIntent renderIntent,
                          ui::Dataspace colorSpaceAgnosticDataspace) {
    ui::Dataspace targetDataspace =
            getDisplayColorProfile()->getTargetDataspace(mode, dataspace,
                                                         colorSpaceAgnosticDataspace);

    if (mState.colorMode == mode && mState.dataspace == dataspace &&
        mState.renderIntent == renderIntent && mState.targetDataspace == targetDataspace) {
        return;
    }

    mState.colorMode = mode;
    mState.dataspace = dataspace;
    mState.renderIntent = renderIntent;
    mState.targetDataspace = targetDataspace;

    mRenderSurface->setBufferDataspace(dataspace);

    ALOGV("Set active color mode: %s (%d), active render intent: %s (%d)",
          decodeColorMode(mode).c_str(), mode, decodeRenderIntent(renderIntent).c_str(),
          renderIntent);

    dirtyEntireOutput();
}

void Output::dump(std::string& out) const {
    using android::base::StringAppendF;

    StringAppendF(&out, "   Composition Output State: [\"%s\"]", mName.c_str());

    out.append("\n   ");

    dumpBase(out);
}

void Output::dumpBase(std::string& out) const {
    mState.dump(out);

    if (mDisplayColorProfile) {
        mDisplayColorProfile->dump(out);
    } else {
        out.append("    No display color profile!\n");
    }

    if (mRenderSurface) {
        mRenderSurface->dump(out);
    } else {
        out.append("    No render surface!\n");
    }

    android::base::StringAppendF(&out, "\n   %zu Layers\b", mOutputLayersOrderedByZ.size());
    for (const auto& outputLayer : mOutputLayersOrderedByZ) {
        if (!outputLayer) {
            continue;
        }
        outputLayer->dump(out);
    }
}

compositionengine::DisplayColorProfile* Output::getDisplayColorProfile() const {
    return mDisplayColorProfile.get();
}

void Output::setDisplayColorProfile(std::unique_ptr<compositionengine::DisplayColorProfile> mode) {
    mDisplayColorProfile = std::move(mode);
}

void Output::setDisplayColorProfileForTest(
        std::unique_ptr<compositionengine::DisplayColorProfile> mode) {
    mDisplayColorProfile = std::move(mode);
}

compositionengine::RenderSurface* Output::getRenderSurface() const {
    return mRenderSurface.get();
}

void Output::setRenderSurface(std::unique_ptr<compositionengine::RenderSurface> surface) {
    mRenderSurface = std::move(surface);
    mState.bounds = Rect(mRenderSurface->getSize());

    dirtyEntireOutput();
}

void Output::setRenderSurfaceForTest(std::unique_ptr<compositionengine::RenderSurface> surface) {
    mRenderSurface = std::move(surface);
}

const OutputCompositionState& Output::getState() const {
    return mState;
}

OutputCompositionState& Output::editState() {
    return mState;
}

Region Output::getDirtyRegion(bool repaintEverything) const {
    Region dirty(mState.viewport);
    if (!repaintEverything) {
        dirty.andSelf(mState.dirtyRegion);
    }
    return dirty;
}

bool Output::belongsInOutput(uint32_t layerStackId, bool internalOnly) const {
    // The layerStackId's must match, and also the layer must not be internal
    // only when not on an internal output.
    return (layerStackId == mState.layerStackId) && (!internalOnly || mState.layerStackInternal);
}

compositionengine::OutputLayer* Output::getOutputLayerForLayer(
        compositionengine::Layer* layer) const {
    for (const auto& outputLayer : mOutputLayersOrderedByZ) {
        if (outputLayer && &outputLayer->getLayer() == layer) {
            return outputLayer.get();
        }
    }
    return nullptr;
}

std::unique_ptr<compositionengine::OutputLayer> Output::getOrCreateOutputLayer(
        std::optional<DisplayId> displayId, std::shared_ptr<compositionengine::Layer> layer,
        sp<compositionengine::LayerFE> layerFE) {
    for (auto& outputLayer : mOutputLayersOrderedByZ) {
        if (outputLayer && &outputLayer->getLayer() == layer.get()) {
            return std::move(outputLayer);
        }
    }
    return createOutputLayer(mCompositionEngine, displayId, *this, layer, layerFE);
}

void Output::setOutputLayersOrderedByZ(OutputLayers&& layers) {
    mOutputLayersOrderedByZ = std::move(layers);
}

const Output::OutputLayers& Output::getOutputLayersOrderedByZ() const {
    return mOutputLayersOrderedByZ;
}

void Output::setReleasedLayers(Output::ReleasedLayers&& layers) {
    mReleasedLayers = std::move(layers);
}

Output::ReleasedLayers Output::takeReleasedLayers() {
    return std::move(mReleasedLayers);
}

void Output::beginFrame() {
    const bool dirty = !getDirtyRegion(false).isEmpty();
    const bool empty = mOutputLayersOrderedByZ.empty();
    const bool wasEmpty = !mState.lastCompositionHadVisibleLayers;

    // If nothing has changed (!dirty), don't recompose.
    // If something changed, but we don't currently have any visible layers,
    //   and didn't when we last did a composition, then skip it this time.
    // The second rule does two things:
    // - When all layers are removed from a display, we'll emit one black
    //   frame, then nothing more until we get new layers.
    // - When a display is created with a private layer stack, we won't
    //   emit any black frames until a layer is added to the layer stack.
    const bool mustRecompose = dirty && !(empty && wasEmpty);

    const char flagPrefix[] = {'-', '+'};
    static_cast<void>(flagPrefix);
    ALOGV_IF("%s: %s composition for %s (%cdirty %cempty %cwasEmpty)", __FUNCTION__,
             mustRecompose ? "doing" : "skipping", getName().c_str(), flagPrefix[dirty],
             flagPrefix[empty], flagPrefix[wasEmpty]);

    mRenderSurface->beginFrame(mustRecompose);

    if (mustRecompose) {
        mState.lastCompositionHadVisibleLayers = !empty;
    }
}

void Output::prepareFrame() {
    ATRACE_CALL();
    ALOGV(__FUNCTION__);

    if (!mState.isEnabled) {
        return;
    }

    chooseCompositionStrategy();

    mRenderSurface->prepareFrame(mState.usesClientComposition, mState.usesDeviceComposition);
}

void Output::postFramebuffer() {
    ATRACE_CALL();
    ALOGV(__FUNCTION__);

    if (!getState().isEnabled) {
        return;
    }

    mRenderSurface->onPresentDisplayCompleted();

    auto frame = presentAndGetFrameFences();

    for (auto& layer : getOutputLayersOrderedByZ()) {
        // The layer buffer from the previous frame (if any) is released
        // by HWC only when the release fence from this frame (if any) is
        // signaled.  Always get the release fence from HWC first.
        sp<Fence> releaseFence = Fence::NO_FENCE;

        if (auto hwcLayer = layer->getHwcLayer()) {
            if (auto f = frame.layerFences.find(hwcLayer); f != frame.layerFences.end()) {
                releaseFence = f->second;
            }
        }

        // If the layer was client composited in the previous frame, we
        // need to merge with the previous client target acquire fence.
        // Since we do not track that, always merge with the current
        // client target acquire fence when it is available, even though
        // this is suboptimal.
        // TODO(b/121291683): Track previous frame client target acquire fence.
        if (mState.usesClientComposition) {
            releaseFence =
                    Fence::merge("LayerRelease", releaseFence, frame.clientTargetAcquireFence);
        }

        layer->getLayerFE().onLayerDisplayed(releaseFence);
    }

    // We've got a list of layers needing fences, that are disjoint with
    // getOutputLayersOrderedByZ.  The best we can do is to
    // supply them with the present fence.
    for (auto& weakLayer : mReleasedLayers) {
        if (auto layer = weakLayer.promote(); layer != nullptr) {
            layer->onLayerDisplayed(frame.presentFence);
        }
    }

    // Clear out the released layers now that we're done with them.
    mReleasedLayers.clear();
}

void Output::dirtyEntireOutput() {
    mState.dirtyRegion.set(mState.bounds);
}

void Output::chooseCompositionStrategy() {
    // The base output implementation can only do client composition
    mState.usesClientComposition = true;
    mState.usesDeviceComposition = false;
}

compositionengine::Output::FrameFences Output::presentAndGetFrameFences() {
    compositionengine::Output::FrameFences result;
    if (mState.usesClientComposition) {
        result.clientTargetAcquireFence = mRenderSurface->getClientTargetAcquireFence();
    }
    return result;
}

} // namespace impl
} // namespace android::compositionengine
